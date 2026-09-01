// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitWorktreeCaptureJournal.h"
#include "GitWorktreeTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    class GitWorktreeCaptureJournalTests
    {
        TEST_CLASS(GitWorktreeCaptureJournalTests);

        TEST_METHOD(CapturesJournalBeforePublishingMarkerAndReloads);
        TEST_METHOD(RecoversMissingMarkerWithoutRecapturingChangedSource);
        TEST_METHOD(RejectsOrphanMarkerWithoutJournalEvidence);
        TEST_METHOD(PropagatesCaptureCancellationWithoutPublishingEvidence);
    };

    void GitWorktreeCaptureJournalTests::CapturesJournalBeforePublishingMarkerAndReloads()
    {
        GitFixture fixture;
        const auto prepared = PrepareTaskWorktree(
            fixture,
            L"capture journal",
            TaskId{ "c1000000-0000-4000-8000-000000000001" });
        GitWorktreeCaptureJournal capture{ fixture.GitExecutable };
        const auto created = capture.CaptureOrLoad(
            prepared.Layout,
            prepared.Marker,
            prepared.Repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(created.Succeeded());
        VERIFY_IS_TRUE(created.Captured);
        VERIFY_IS_FALSE(created.RecoveredMarker);
        VERIFY_ARE_EQUAL(
            created.Journal->SnapshotDigest,
            created.Marker->SnapshotDigest);
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(
            prepared.Layout.SnapshotPath));
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(
            prepared.Layout.MarkerPath));

        const auto loaded = capture.CaptureOrLoad(
            prepared.Layout,
            prepared.Marker,
            prepared.Repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(loaded.Succeeded());
        VERIFY_IS_FALSE(loaded.Captured);
        VERIFY_IS_FALSE(loaded.RecoveredMarker);
        VERIFY_ARE_EQUAL(
            created.Journal->SnapshotDigest,
            loaded.Journal->SnapshotDigest);
    }

    void GitWorktreeCaptureJournalTests::RecoversMissingMarkerWithoutRecapturingChangedSource()
    {
        GitFixture fixture;
        const auto prepared = PrepareTaskWorktree(
            fixture,
            L"marker recovery",
            TaskId{ "c2000000-0000-4000-8000-000000000002" });
        VERIFY_IS_TRUE(CreateGitWorktreeSourceJournal(
                           prepared.Layout,
                           prepared.Marker,
                           prepared.Journal)
                           .Succeeded());
        GitFixture::Write(prepared.Repository / L"later.txt", "later source\n");

        GitWorktreeCaptureJournal capture{ fixture.GitExecutable };
        const auto recovered = capture.CaptureOrLoad(
            prepared.Layout,
            prepared.Marker,
            prepared.Repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_FALSE(recovered.Captured);
        VERIFY_IS_TRUE(recovered.RecoveredMarker);
        VERIFY_ARE_EQUAL(
            prepared.Journal.SnapshotDigest,
            recovered.Journal->SnapshotDigest);
        VERIFY_ARE_EQUAL(
            prepared.Journal.SnapshotDigest,
            recovered.Marker->SnapshotDigest);
        VERIFY_ARE_EQUAL(size_t{ 0 }, recovered.Journal->UntrackedFiles.size());
    }

    void GitWorktreeCaptureJournalTests::RejectsOrphanMarkerWithoutJournalEvidence()
    {
        GitFixture fixture;
        const auto prepared = PrepareTaskWorktree(
            fixture,
            L"orphan marker",
            TaskId{ "c3000000-0000-4000-8000-000000000003" });
        VERIFY_IS_TRUE(CreateGitWorktreeMarker(
                           prepared.Layout,
                           prepared.Marker)
                           .Succeeded());
        GitWorktreeCaptureJournal capture{ fixture.GitExecutable };
        const auto result = capture.CaptureOrLoad(
            prepared.Layout,
            prepared.Marker,
            prepared.Repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(
            result.Status ==
            GitWorktreeCaptureJournalStatus::EvidenceIncomplete);
        VERIFY_IS_FALSE(std::filesystem::exists(
            prepared.Layout.SnapshotPath));
    }

    void GitWorktreeCaptureJournalTests::PropagatesCaptureCancellationWithoutPublishingEvidence()
    {
        GitFixture fixture;
        const auto prepared = PrepareTaskWorktree(
            fixture,
            L"cancelled capture journal",
            TaskId{ "c4000000-0000-4000-8000-000000000004" });
        const auto cancellation = MakeEvent(true);
        VERIFY_IS_TRUE(static_cast<bool>(cancellation));
        GitWorktreeCaptureJournal capture{ fixture.GitExecutable };
        const auto result = capture.CaptureOrLoad(
            prepared.Layout,
            prepared.Marker,
            prepared.Repository,
            cancellation.Get());
        VERIFY_IS_TRUE(
            result.Status == GitWorktreeCaptureJournalStatus::CaptureFailed);
        VERIFY_IS_TRUE(result.CaptureStatus == GitSourceCaptureStatus::Cancelled);
        VERIFY_IS_FALSE(std::filesystem::exists(
            prepared.Layout.SnapshotPath));
        VERIFY_IS_FALSE(std::filesystem::exists(
            prepared.Layout.MarkerPath));
    }
}
