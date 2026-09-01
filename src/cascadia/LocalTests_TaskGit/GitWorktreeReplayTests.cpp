// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceCaptureTestSupport.h"
#include "GitWorktreeReplay.h"
#include "GitWorktreeTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    class GitWorktreeReplayTests
    {
        TEST_CLASS(GitWorktreeReplayTests);

        TEST_METHOD(ReplaysStagedUnstagedAndUntrackedStateWithoutIgnoredFiles);
        TEST_METHOD(RetryReusesCopiedFilesAfterSourceFailure);
        TEST_METHOD(RejectsUnrelatedTargetChangesWithoutOverwritingThem);
        TEST_METHOD(PreservesTrackedStateWhenStashApplyFailsAndRecovers);
        TEST_METHOD(ReplaysCleanSha256SnapshotIdempotently);
    };

    void GitWorktreeReplayTests::ReplaysStagedUnstagedAndUntrackedStateWithoutIgnoredFiles()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"dirty replay",
            TaskId{ "a1000000-0000-4000-8000-000000000001" });
        GitFixture::Write(prepared.Repository / L".gitignore", "*.ignored\n");
        VERIFY_IS_TRUE(CommitAll(fixture, prepared.Repository, L"add ignore rules"));
        GitFixture::Write(prepared.Repository / L"tracked.txt", "staged state\n");
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"add", L"--", L"tracked.txt" })
                           .Succeeded());
        GitFixture::Write(prepared.Repository / L"tracked.txt", "worktree state\n");
        const auto nested = prepared.Repository / L"嵌套";
        VERIFY_IS_TRUE(std::filesystem::create_directory(nested));
        GitFixture::Write(nested / L"文件.txt", "untracked state\n");
        GitFixture::Write(prepared.Repository / L"cache.ignored", "ignored state\n");
        RecaptureTaskWorktree(fixture, prepared);
        const auto before = ObserveGitSourceState(
            fixture,
            prepared.Repository,
            {
                L"tracked.txt",
                std::filesystem::path{ L"嵌套" } / L"文件.txt",
                L"cache.ignored",
            });
        PersistTaskWorktreeEvidence(prepared);
        VERIFY_IS_TRUE(ProvisionTaskWorktree(
                           fixture,
                           prepared,
                           GitWorktreePreflightMode::RequireUnoccupied)
                           .Succeeded());

        const auto replayed = ReplayTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(replayed.Succeeded());
        VERIFY_IS_TRUE(replayed.TrackedApplied);
        VERIFY_ARE_EQUAL(size_t{ 1 }, replayed.FilesCopied);
        VERIFY_ARE_EQUAL(size_t{ 0 }, replayed.FilesReused);
        VERIFY_ARE_EQUAL(
            std::string{ "worktree state\n" },
            ReadBinaryFile(prepared.Layout.WorktreePath / L"tracked.txt"));
        VERIFY_ARE_EQUAL(
            std::string{ "untracked state\n" },
            ReadBinaryFile(
                prepared.Layout.WorktreePath / L"嵌套" / L"文件.txt"));
        VERIFY_IS_FALSE(std::filesystem::exists(
            prepared.Layout.WorktreePath / L"cache.ignored"));
        VERIFY_ARE_EQUAL(
            std::string{ "staged state" },
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"show", L":tracked.txt" })));

        const auto after = ObserveGitSourceState(
            fixture,
            prepared.Repository,
            {
                L"tracked.txt",
                std::filesystem::path{ L"嵌套" } / L"文件.txt",
                L"cache.ignored",
            });
        VERIFY_IS_TRUE(SameGitSourceState(before, after));

        const auto resumed = ReplayTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(resumed.Succeeded());
        VERIFY_IS_FALSE(resumed.TrackedApplied);
        VERIFY_ARE_EQUAL(size_t{ 0 }, resumed.FilesCopied);
        VERIFY_ARE_EQUAL(size_t{ 1 }, resumed.FilesReused);
    }

    void GitWorktreeReplayTests::RetryReusesCopiedFilesAfterSourceFailure()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"copy retry",
            TaskId{ "a2000000-0000-4000-8000-000000000002" });
        GitFixture::Write(prepared.Repository / L"a.txt", "first\n");
        GitFixture::Write(prepared.Repository / L"b.txt", "second\n");
        RecaptureTaskWorktree(fixture, prepared);
        PersistTaskWorktreeEvidence(prepared);
        VERIFY_IS_TRUE(ProvisionTaskWorktree(
                           fixture,
                           prepared,
                           GitWorktreePreflightMode::RequireUnoccupied)
                           .Succeeded());
        GitFixture::Write(prepared.Repository / L"b.txt", "changed\n");

        const auto failed = ReplayTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(failed.Status == GitWorktreeReplayStatus::CopyFailed);
        VERIFY_IS_TRUE(
            failed.FileCopyStatus == GitWorktreeFileCopyStatus::SourceChanged);
        VERIFY_ARE_EQUAL(std::string{ "b.txt" }, failed.FailurePath);
        VERIFY_ARE_EQUAL(size_t{ 1 }, failed.FilesCopied);
        VERIFY_IS_TRUE(failed.Preserved);
        VERIFY_ARE_EQUAL(
            std::string{ "first\n" },
            ReadBinaryFile(prepared.Layout.WorktreePath / L"a.txt"));
        VERIFY_IS_FALSE(std::filesystem::exists(
            prepared.Layout.WorktreePath / L"b.txt"));

        GitFixture::Write(prepared.Repository / L"b.txt", "second\n");
        const auto recovered = ReplayTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, recovered.FilesCopied);
        VERIFY_ARE_EQUAL(size_t{ 1 }, recovered.FilesReused);
    }

    void GitWorktreeReplayTests::RejectsUnrelatedTargetChangesWithoutOverwritingThem()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"target collision",
            TaskId{ "a3000000-0000-4000-8000-000000000003" });
        PersistTaskWorktreeEvidence(prepared);
        VERIFY_IS_TRUE(ProvisionTaskWorktree(
                           fixture,
                           prepared,
                           GitWorktreePreflightMode::RequireUnoccupied)
                           .Succeeded());
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "unrelated target edit\n");

        const auto result = ReplayTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(result.Status == GitWorktreeReplayStatus::TargetChanged);
        VERIFY_IS_TRUE(result.Preserved);
        VERIFY_ARE_EQUAL(
            std::string{ "unrelated target edit\n" },
            ReadBinaryFile(prepared.Layout.WorktreePath / L"tracked.txt"));
    }

    void GitWorktreeReplayTests::PreservesTrackedStateWhenStashApplyFailsAndRecovers()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"apply failure",
            TaskId{ "a4000000-0000-4000-8000-000000000004" });
        GitFixture::Write(prepared.Repository / L"tracked.txt", "captured edit\n");
        RecaptureTaskWorktree(fixture, prepared);
        PersistTaskWorktreeEvidence(prepared);
        VERIFY_IS_TRUE(ProvisionTaskWorktree(
                           fixture,
                           prepared,
                           GitWorktreePreflightMode::RequireUnoccupied)
                           .Succeeded());

        GitCommandRunner runner;
        GitWorktreeReplayer failing{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                const auto stash = std::ranges::find(
                    request.Arguments,
                    L"stash");
                const auto apply = std::ranges::find(
                    request.Arguments,
                    L"apply");
                if (stash != request.Arguments.end() &&
                    apply != request.Arguments.end())
                {
                    GitCommandResult result;
                    result.Status = GitCommandStatus::Completed;
                    result.ExitCode = 1;
                    result.Stderr = "injected stash apply failure";
                    return result;
                }
                return runner.Run(request);
            }
        };
        const auto failed = failing.Replay(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(
            failed.Status == GitWorktreeReplayStatus::TrackedApplyFailed);
        VERIFY_IS_TRUE(failed.Preserved);
        VERIFY_ARE_EQUAL(
            std::string{ "base\n" },
            ReadBinaryFile(prepared.Layout.WorktreePath / L"tracked.txt"));

        const auto recovered = ReplayTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(recovered.TrackedApplied);
        VERIFY_ARE_EQUAL(
            std::string{ "captured edit\n" },
            ReadBinaryFile(prepared.Layout.WorktreePath / L"tracked.txt"));
    }

    void GitWorktreeReplayTests::ReplaysCleanSha256SnapshotIdempotently()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"clean sha256 replay",
            TaskId{ "a5000000-0000-4000-8000-000000000005" },
            true);
        PersistTaskWorktreeEvidence(prepared);
        VERIFY_IS_TRUE(ProvisionTaskWorktree(
                           fixture,
                           prepared,
                           GitWorktreePreflightMode::RequireUnoccupied)
                           .Succeeded());

        const auto cancellation = MakeEvent(true);
        VERIFY_IS_TRUE(static_cast<bool>(cancellation));
        const auto cancelled = ReplayTaskWorktree(
            fixture,
            prepared,
            cancellation.Get());
        VERIFY_IS_TRUE(cancelled.Status == GitWorktreeReplayStatus::Cancelled);
        VERIFY_IS_TRUE(cancelled.Preserved);

        auto result = ReplayTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_FALSE(result.TrackedApplied);
        VERIFY_ARE_EQUAL(size_t{ 0 }, result.FilesCopied);
        VERIFY_ARE_EQUAL(size_t{ 0 }, result.FilesReused);
        result = ReplayTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_FALSE(result.TrackedApplied);
    }
}
