// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceSnapshot.h"
#include "GitWorktreeMarker.h"
#include "TaskGitTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    namespace
    {
        const TaskId TestTask{
            "33333333-3333-4333-8333-333333333333"
        };
        constexpr std::string_view OwnershipNonce{
            "44444444-4444-4444-8444-444444444444"
        };

        GitWorktreeMarker _marker(const GitWorktreeLayout& layout)
        {
            GitWorktreeMarker marker;
            marker.Task = layout.Task;
            marker.RepositoryIdentityDigest.assign(64, 'a');
            marker.BranchRef = layout.BranchRef;
            marker.OwnershipNonce = OwnershipNonce;
            marker.WorktreePath = layout.WorktreePathUtf8;
            return marker;
        }

        bool _sameMarker(
            const GitWorktreeMarker& left,
            const GitWorktreeMarker& right)
        {
            return left.FormatVersion == right.FormatVersion &&
                   left.Task == right.Task &&
                   left.RepositoryIdentityDigest == right.RepositoryIdentityDigest &&
                   left.BranchRef == right.BranchRef &&
                   left.SnapshotDigest == right.SnapshotDigest &&
                   left.BaselineCommit == right.BaselineCommit &&
                   left.OwnershipNonce == right.OwnershipNonce &&
                   left.WorktreePath == right.WorktreePath;
        }
    }

    class GitWorktreeMarkerTests
    {
        TEST_CLASS(GitWorktreeMarkerTests);

        TEST_METHOD(CodecRoundTripsDeterministicallyAndRejectsCorruption);
        TEST_METHOD(CreatesReadsAndAtomicallyAdvancesMarkerEvidence);
        TEST_METHOD(RejectsInvalidIdentityStaleReplacementAndUnsafeFile);
    };

    void GitWorktreeMarkerTests::CodecRoundTripsDeterministicallyAndRejectsCorruption()
    {
        TemporaryDirectory temporary;
        const auto layout = PrepareGitWorktreeLayout(temporary.Path(), TestTask);
        VERIFY_IS_TRUE(layout.Succeeded());
        auto marker = _marker(*layout.Layout);
        marker.SnapshotDigest.assign(64, 'b');
        marker.BaselineCommit.assign(40, 'c');

        const auto first = details::EncodeGitWorktreeMarker(marker);
        const auto second = details::EncodeGitWorktreeMarker(marker);
        VERIFY_IS_TRUE(first.has_value());
        VERIFY_IS_TRUE(second.has_value());
        VERIFY_IS_TRUE(*first == *second);
        VERIFY_IS_LESS_THAN(first->size(), MaximumGitWorktreeMarkerBytes);
        const auto decoded = details::DecodeGitWorktreeMarker(*first);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_IS_TRUE(_sameMarker(marker, *decoded.Marker));

        auto corrupted = *first;
        corrupted[20] ^= 0x01;
        VERIFY_IS_TRUE(
            details::DecodeGitWorktreeMarker(corrupted).Status ==
            GitWorktreeMarkerStatus::Malformed);
        auto truncated = *first;
        truncated.pop_back();
        VERIFY_IS_TRUE(
            details::DecodeGitWorktreeMarker(truncated).Status ==
            GitWorktreeMarkerStatus::Malformed);
        auto trailing = *first;
        trailing.push_back(0);
        VERIFY_IS_TRUE(
            details::DecodeGitWorktreeMarker(trailing).Status ==
            GitWorktreeMarkerStatus::Malformed);

        auto invalid = marker;
        invalid.OwnershipNonce = "not-a-uuid";
        VERIFY_IS_FALSE(details::EncodeGitWorktreeMarker(invalid).has_value());
        invalid = marker;
        invalid.WorktreePath.assign(MaximumGitSnapshotPathBytes + 1, 'x');
        VERIFY_IS_FALSE(details::EncodeGitWorktreeMarker(invalid).has_value());
    }

    void GitWorktreeMarkerTests::CreatesReadsAndAtomicallyAdvancesMarkerEvidence()
    {
        TemporaryDirectory temporary;
        const auto layout = PrepareGitWorktreeLayout(temporary.Path(), TestTask);
        VERIFY_IS_TRUE(layout.Succeeded());
        const auto initial = _marker(*layout.Layout);
        VERIFY_IS_TRUE(
            ReadGitWorktreeMarker(*layout.Layout).Status ==
            GitWorktreeMarkerStatus::Missing);

        const auto created = CreateGitWorktreeMarker(*layout.Layout, initial);
        VERIFY_IS_TRUE(created.Succeeded());
        VERIFY_IS_TRUE(_sameMarker(initial, *created.Marker));
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(
            layout.Layout->MarkerPath));
        VERIFY_IS_TRUE(
            CreateGitWorktreeMarker(*layout.Layout, initial).Status ==
            GitWorktreeMarkerStatus::Collision);

        auto captured = initial;
        captured.SnapshotDigest.assign(64, 'b');
        const auto capturedResult = ReplaceGitWorktreeMarker(
            *layout.Layout,
            initial,
            captured);
        VERIFY_IS_TRUE(capturedResult.Succeeded());
        VERIFY_IS_TRUE(_sameMarker(captured, *capturedResult.Marker));

        auto ready = captured;
        ready.BaselineCommit.assign(40, 'c');
        const auto readyResult = ReplaceGitWorktreeMarker(
            *layout.Layout,
            captured,
            ready);
        VERIFY_IS_TRUE(readyResult.Succeeded());
        VERIFY_IS_TRUE(_sameMarker(ready, *readyResult.Marker));
        const auto loaded = ReadGitWorktreeMarker(*layout.Layout);
        VERIFY_IS_TRUE(loaded.Succeeded());
        VERIFY_IS_TRUE(_sameMarker(ready, *loaded.Marker));

        size_t metadataFiles{};
        for (const auto& entry :
             std::filesystem::directory_iterator(
                 layout.Layout->StateRoot.CanonicalPath()))
        {
            if (entry.is_regular_file())
            {
                ++metadataFiles;
            }
        }
        VERIFY_ARE_EQUAL(size_t{ 1 }, metadataFiles);
    }

    void GitWorktreeMarkerTests::RejectsInvalidIdentityStaleReplacementAndUnsafeFile()
    {
        TemporaryDirectory temporary;
        const auto layout = PrepareGitWorktreeLayout(temporary.Path(), TestTask);
        VERIFY_IS_TRUE(layout.Succeeded());
        const auto marker = _marker(*layout.Layout);

        auto wrongBranch = marker;
        wrongBranch.BranchRef = "refs/heads/wt-task/wrong";
        VERIFY_IS_FALSE(ValidateGitWorktreeMarker(*layout.Layout, wrongBranch));
        VERIFY_IS_TRUE(
            CreateGitWorktreeMarker(*layout.Layout, wrongBranch).Status ==
            GitWorktreeMarkerStatus::InvalidRequest);
        VERIFY_IS_TRUE(CreateGitWorktreeMarker(*layout.Layout, marker).Succeeded());

        auto replacement = marker;
        replacement.SnapshotDigest.assign(64, 'b');
        auto stale = marker;
        stale.RepositoryIdentityDigest.assign(64, 'd');
        VERIFY_IS_TRUE(
            ReplaceGitWorktreeMarker(*layout.Layout, stale, replacement).Status ==
            GitWorktreeMarkerStatus::InvalidRequest);
        stale = marker;
        stale.SnapshotDigest.assign(64, 'b');
        replacement = stale;
        replacement.BaselineCommit.assign(40, 'c');
        VERIFY_IS_TRUE(
            ReplaceGitWorktreeMarker(*layout.Layout, stale, replacement).Status ==
            GitWorktreeMarkerStatus::IdentityMismatch);

        VERIFY_IS_TRUE(std::filesystem::remove(layout.Layout->MarkerPath));
        const auto target = temporary.Path() / L"outside-marker.bin";
        GitFixture::Write(target, "outside\n");
        if (CreateSymbolicLinkW(
                layout.Layout->MarkerPath.c_str(),
                target.c_str(),
                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE)
        {
            WEX::Logging::Log::Comment(WEX::Common::String().Format(
                L"File symlink creation unavailable; integration check skipped (error=%lu).",
                GetLastError()));
            return;
        }
        VERIFY_IS_TRUE(
            ReadGitWorktreeMarker(*layout.Layout).Status ==
            GitWorktreeMarkerStatus::UnsafePath);
    }
}
