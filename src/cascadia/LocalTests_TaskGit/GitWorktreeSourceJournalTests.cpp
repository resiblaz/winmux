// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceCapture.h"
#include "GitWorktreeSourceJournal.h"
#include "TaskGitTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    namespace
    {
        const TaskId TestTask{
            "55555555-5555-4555-8555-555555555555"
        };
        constexpr std::string_view OwnershipNonce{
            "66666666-6666-4666-8666-666666666666"
        };

        GitWorktreeMarker _marker(
            const GitWorktreeLayout& layout,
            const std::string& repositoryDigest)
        {
            GitWorktreeMarker marker;
            marker.Task = layout.Task;
            marker.RepositoryIdentityDigest = repositoryDigest;
            marker.BranchRef = layout.BranchRef;
            marker.OwnershipNonce = OwnershipNonce;
            marker.WorktreePath = layout.WorktreePathUtf8;
            return marker;
        }

        GitWorktreeSourceJournal _journal(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker)
        {
            GitWorktreeSourceJournal journal;
            journal.Task = layout.Task;
            journal.OwnershipNonce = marker.OwnershipNonce;
            journal.RepositoryIdentityDigest = marker.RepositoryIdentityDigest;
            journal.SourceWorktreePath = layout.WorktreePathUtf8;
            journal.ObjectFormat = GitObjectFormat::Sha1;
            journal.SourceHead.assign(40, 'a');
            journal.SourceReference = "refs/heads/main";
            journal.WorktreeTree.assign(40, 'b');
            journal.IndexTree.assign(40, 'c');
            journal.StatusDigest.assign(64, 'd');
            journal.SnapshotDigest.assign(64, 'e');
            journal.UntrackedFiles = {
                { "a.txt", GitSourceFileType::RegularFile, 2, std::string(64, '1') },
                { "folder/b.txt", GitSourceFileType::RegularFile, 3, std::string(64, '2') },
            };
            journal.UntrackedBytes = 5;
            return journal;
        }

        bool _sameJournal(
            const GitWorktreeSourceJournal& left,
            const GitWorktreeSourceJournal& right)
        {
            const auto leftBytes = details::EncodeGitWorktreeSourceJournal(left);
            const auto rightBytes = details::EncodeGitWorktreeSourceJournal(right);
            return leftBytes && rightBytes && *leftBytes == *rightBytes;
        }
    }

    class GitWorktreeSourceJournalTests
    {
        TEST_CLASS(GitWorktreeSourceJournalTests);

        TEST_METHOD(CodecRoundTripsAndRejectsCorruptionAndInvalidManifest);
        TEST_METHOD(PersistsRealCapturedSnapshotAndBindsMarker);
        TEST_METHOD(RejectsWrongOwnershipAndCorruptedFile);
    };

    void GitWorktreeSourceJournalTests::CodecRoundTripsAndRejectsCorruptionAndInvalidManifest()
    {
        TemporaryDirectory temporary;
        const auto layout = PrepareGitWorktreeLayout(temporary.Path(), TestTask);
        VERIFY_IS_TRUE(layout.Succeeded());
        const auto marker = _marker(*layout.Layout, std::string(64, 'a'));
        const auto journal = _journal(*layout.Layout, marker);

        const auto first = details::EncodeGitWorktreeSourceJournal(journal);
        const auto second = details::EncodeGitWorktreeSourceJournal(journal);
        VERIFY_IS_TRUE(first.has_value());
        VERIFY_IS_TRUE(second.has_value());
        VERIFY_IS_TRUE(*first == *second);
        const auto decoded = details::DecodeGitWorktreeSourceJournal(*first);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_IS_TRUE(_sameJournal(journal, *decoded.Journal));

        auto corrupted = *first;
        corrupted[24] ^= 0x40;
        VERIFY_IS_TRUE(
            details::DecodeGitWorktreeSourceJournal(corrupted).Status ==
            GitWorktreeSourceJournalStatus::Malformed);
        auto truncated = *first;
        truncated.resize(truncated.size() / 2);
        VERIFY_IS_TRUE(
            details::DecodeGitWorktreeSourceJournal(truncated).Status ==
            GitWorktreeSourceJournalStatus::Malformed);

        auto invalid = journal;
        invalid.UntrackedBytes = 4;
        VERIFY_IS_FALSE(
            details::ValidateGitWorktreeSourceJournalShape(invalid));
        invalid = journal;
        invalid.UntrackedFiles[1].RelativePath = "a.txt";
        VERIFY_IS_FALSE(
            details::ValidateGitWorktreeSourceJournalShape(invalid));
        invalid = journal;
        invalid.UntrackedFiles[0].Type =
            static_cast<GitSourceFileType>(255);
        VERIFY_IS_FALSE(
            details::EncodeGitWorktreeSourceJournal(invalid).has_value());
    }

    void GitWorktreeSourceJournalTests::PersistsRealCapturedSnapshotAndBindsMarker()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"source repository";
        const auto runtimeRoot = fixture.Temporary.Path() / L"runtime data";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        VERIFY_IS_TRUE(std::filesystem::create_directory(runtimeRoot));
        const auto nested = repository / L"嵌套";
        VERIFY_IS_TRUE(std::filesystem::create_directory(nested));
        GitFixture::Write(nested / L"文件.txt", "journal\n");

        GitSourceCapture capture{ fixture.GitExecutable };
        const auto snapshot = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(snapshot.Succeeded());
        const auto layout = PrepareGitWorktreeLayout(runtimeRoot, TestTask);
        VERIFY_IS_TRUE(layout.Succeeded());
        auto marker = _marker(
            *layout.Layout,
            snapshot.Snapshot->Repository.CommonDirectoryIdentity.Digest());
        VERIFY_IS_TRUE(CreateGitWorktreeMarker(*layout.Layout, marker).Succeeded());

        const auto made = MakeGitWorktreeSourceJournal(
            *layout.Layout,
            marker,
            *snapshot.Snapshot);
        VERIFY_IS_TRUE(made.Succeeded());
        VERIFY_ARE_EQUAL(
            snapshot.Snapshot->SnapshotDigest,
            made.Journal->SnapshotDigest);
        VERIFY_ARE_EQUAL(size_t{ 1 }, made.Journal->UntrackedFiles.size());

        auto inconsistent = *snapshot.Snapshot;
        inconsistent.SnapshotDigest.assign(64, '0');
        VERIFY_IS_TRUE(
            MakeGitWorktreeSourceJournal(
                *layout.Layout,
                marker,
                inconsistent)
                .Status == GitWorktreeSourceJournalStatus::InvalidRequest);

        const auto created = CreateGitWorktreeSourceJournal(
            *layout.Layout,
            marker,
            *made.Journal);
        VERIFY_IS_TRUE(created.Succeeded());
        VERIFY_IS_TRUE(_sameJournal(*made.Journal, *created.Journal));
        VERIFY_IS_TRUE(
            CreateGitWorktreeSourceJournal(
                *layout.Layout,
                marker,
                *made.Journal)
                    .Status == GitWorktreeSourceJournalStatus::Collision);

        auto capturedMarker = marker;
        capturedMarker.SnapshotDigest = made.Journal->SnapshotDigest;
        VERIFY_IS_TRUE(ReplaceGitWorktreeMarker(
                           *layout.Layout,
                           marker,
                           capturedMarker)
                           .Succeeded());
        const auto loaded = ReadGitWorktreeSourceJournal(
            *layout.Layout,
            capturedMarker);
        VERIFY_IS_TRUE(loaded.Succeeded());
        VERIFY_IS_TRUE(_sameJournal(*made.Journal, *loaded.Journal));
    }

    void GitWorktreeSourceJournalTests::RejectsWrongOwnershipAndCorruptedFile()
    {
        TemporaryDirectory temporary;
        const auto layout = PrepareGitWorktreeLayout(temporary.Path(), TestTask);
        VERIFY_IS_TRUE(layout.Succeeded());
        const auto marker = _marker(*layout.Layout, std::string(64, 'a'));
        const auto journal = _journal(*layout.Layout, marker);
        VERIFY_IS_TRUE(CreateGitWorktreeMarker(*layout.Layout, marker).Succeeded());
        VERIFY_IS_TRUE(CreateGitWorktreeSourceJournal(
                           *layout.Layout,
                           marker,
                           journal)
                           .Succeeded());

        auto wrongOwner = marker;
        wrongOwner.OwnershipNonce =
            "77777777-7777-4777-8777-777777777777";
        VERIFY_IS_TRUE(
            ReadGitWorktreeSourceJournal(*layout.Layout, wrongOwner).Status ==
            GitWorktreeSourceJournalStatus::IdentityMismatch);

        GitFixture::Write(layout.Layout->SnapshotPath, "corrupted\n");
        VERIFY_IS_TRUE(
            ReadGitWorktreeSourceJournal(*layout.Layout, marker).Status ==
            GitWorktreeSourceJournalStatus::Malformed);
    }
}
