// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceCaptureTestSupport.h"
#include "GitWorktreeCheckpoint.h"
#include "GitWorktreeTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    namespace
    {
        GitWorktreeCheckpointManager _manager(const GitFixture& fixture)
        {
            return GitWorktreeCheckpointManager{ fixture.GitExecutable };
        }

        std::string _head(
            const GitFixture& fixture,
            const PreparedTaskWorktree& prepared)
        {
            return SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"rev-parse", L"--verify", L"HEAD" }));
        }

        void _commitAgentChange(
            const GitFixture& fixture,
            const PreparedTaskWorktree& prepared,
            const std::wstring_view subject)
        {
            VERIFY_IS_TRUE(fixture.Run(
                                      prepared.Layout.WorktreePath,
                                      { L"add", L"-A", L"--" })
                               .Succeeded());
            VERIFY_IS_TRUE(fixture.Run(
                                      prepared.Layout.WorktreePath,
                                      {
                                          L"-c",
                                          L"user.name=Agent Test",
                                          L"-c",
                                          L"user.email=agent@example.invalid",
                                          L"-c",
                                          L"commit.gpgSign=false",
                                          L"-c",
                                          L"core.hooksPath=NUL",
                                          L"commit",
                                          L"--no-verify",
                                          L"-m",
                                          std::wstring{ subject },
                                      })
                               .Succeeded());
        }
    }

    class GitWorktreeCheckpointTests
    {
        TEST_CLASS(GitWorktreeCheckpointTests);

        TEST_METHOD(PreservesAgentCommitsAndCheckpointsDirtyRemainderExactlyOnce);
        TEST_METHOD(DoesNotCreateEmptyCommitAndReusesWorktreeForNextRun);
        TEST_METHOD(DisablesHooksUsesFallbackIdentityAndSupportsSha256);
        TEST_METHOD(PreservesSigningFailureAndRetriesWithoutLosingStagedWork);
        TEST_METHOD(RecoversCommitAfterPostCommitObservationFailure);
    };

    void GitWorktreeCheckpointTests::PreservesAgentCommitsAndCheckpointsDirtyRemainderExactlyOnce()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"checkpoint output",
            TaskId{ "c1000000-0000-4000-8000-000000000001" });
        PrepareReadyTaskWorktree(fixture, prepared);
        auto manager = _manager(fixture);
        const auto start = manager.CaptureStart(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(start.Succeeded());
        VERIFY_ARE_EQUAL(_head(fixture, prepared), start.StartCommit);
        VERIFY_ARE_EQUAL(
            start.StateDigest,
            *ParseGitWorktreeCheckpointStartDigest(start.DiffSummaryJson));

        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "agent commit\n");
        _commitAgentChange(fixture, prepared, L"agent authored commit");
        const auto agentCommit = _head(fixture, prepared);
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "dirty remainder\n");
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"新文件.txt",
            "new output\n");

        const auto inspected = manager.InspectAndStage(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            start.StartCommit,
            start.StateDigest,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(inspected.Succeeded());
        VERIFY_IS_TRUE(inspected.NeedsCommit);
        VERIFY_ARE_EQUAL(agentCommit, inspected.AgentEndCommit);
        VERIFY_IS_TRUE(inspected.ChangedPathsJson.find("tracked.txt") != std::string::npos);
        VERIFY_IS_TRUE(inspected.ChangedPathsJson.find("新文件.txt") != std::string::npos);

        const TaskRunId run{ "d1000000-0000-4000-8000-000000000001" };
        const auto committed = manager.CommitOrRecover(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            run,
            inspected.AgentEndCommit,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(committed.Succeeded());
        VERIFY_IS_TRUE(committed.CommitCreated);
        VERIFY_ARE_NOT_EQUAL(agentCommit, committed.CheckpointCommit);
        VERIFY_ARE_EQUAL(
            agentCommit,
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"rev-parse", L"HEAD^" })));
        VERIFY_ARE_EQUAL(
            std::string{ "agent authored commit" },
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"show", L"-s", L"--format=%s", L"HEAD^" })));

        const auto recovered = manager.CommitOrRecover(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            run,
            inspected.AgentEndCommit,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(
            recovered.Status == GitWorktreeCheckpointStatus::AlreadyCommitted);
        VERIFY_IS_FALSE(recovered.CommitCreated);
        VERIFY_ARE_EQUAL(committed.CheckpointCommit, recovered.CheckpointCommit);
    }

    void GitWorktreeCheckpointTests::DoesNotCreateEmptyCommitAndReusesWorktreeForNextRun()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"empty checkpoint",
            TaskId{ "c2000000-0000-4000-8000-000000000002" });
        PrepareReadyTaskWorktree(fixture, prepared);
        auto manager = _manager(fixture);
        const auto first = manager.CaptureStart(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(first.Succeeded());
        const auto inspected = manager.InspectAndStage(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            first.StartCommit,
            first.StateDigest,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(inspected.Succeeded());
        VERIFY_IS_FALSE(inspected.NeedsCommit);
        VERIFY_ARE_EQUAL(std::string{ "[]" }, inspected.ChangedPathsJson);
        VERIFY_ARE_EQUAL(first.StartCommit, inspected.AgentEndCommit);
        VERIFY_ARE_EQUAL(first.StartCommit, _head(fixture, prepared));

        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "second run\n");
        const auto dirty = manager.CaptureStart(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(
            dirty.Status == GitWorktreeCheckpointStatus::TargetChanged);
        VERIFY_IS_TRUE(dirty.Preserved);
        VERIFY_ARE_EQUAL(prepared.Layout.WorktreePathUtf8, dirty.PreservedPath);
    }

    void GitWorktreeCheckpointTests::DisablesHooksUsesFallbackIdentityAndSupportsSha256()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"fallback checkpoint",
            TaskId{ "c3000000-0000-4000-8000-000000000003" },
            true);
        PrepareReadyTaskWorktree(fixture, prepared);
        const auto hooks = fixture.Temporary.Path() / L"checkpoint hooks";
        VERIFY_IS_TRUE(std::filesystem::create_directory(hooks));
        const auto preCommit = hooks / L"pre-commit";
        GitFixture::Write(preCommit, "#!/bin/sh\nexit 91\n");
        std::filesystem::permissions(
            preCommit,
            std::filesystem::perms::owner_exec |
                std::filesystem::perms::group_exec |
                std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"core.hooksPath", hooks.native() })
                           .Succeeded());
        VERIFY_ARE_EQUAL(
            DWORD{ 91 },
            fixture.Run(
                       prepared.Layout.WorktreePath,
                       { L"hook", L"run", L"pre-commit" })
                .ExitCode);

        GitCommandRunner runner;
        GitWorktreeCheckpointManager manager{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                if (request.Arguments ==
                        std::vector<std::wstring>{ L"config", L"--get", L"user.name" } ||
                    request.Arguments ==
                        std::vector<std::wstring>{ L"config", L"--get", L"user.email" })
                {
                    GitCommandResult result;
                    result.Status = GitCommandStatus::Completed;
                    result.ExitCode = 1;
                    return result;
                }
                return runner.Run(request);
            }
        };
        const auto start = manager.CaptureStart(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(start.Succeeded());
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "fallback identity\n");
        const auto inspected = manager.InspectAndStage(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            start.StartCommit,
            start.StateDigest,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(inspected.Succeeded());
        const auto committed = manager.CommitOrRecover(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            TaskRunId{ "d3000000-0000-4000-8000-000000000003" },
            inspected.AgentEndCommit,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(committed.Succeeded());
        VERIFY_IS_TRUE(committed.UsedFallbackIdentity);
        VERIFY_ARE_EQUAL(size_t{ 64 }, committed.CheckpointCommit.size());
        VERIFY_ARE_EQUAL(
            std::string{ "Richer Task Runtime <task-runtime@localhost>" },
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"show", L"-s", L"--format=%an <%ae>", L"HEAD" })));
    }

    void GitWorktreeCheckpointTests::PreservesSigningFailureAndRetriesWithoutLosingStagedWork()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"sign checkpoint",
            TaskId{ "c4000000-0000-4000-8000-000000000004" });
        PrepareReadyTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"user.name", L"Signing Test" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"user.email", L"signing@example.invalid" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"commit.gpgSign", L"true" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"gpg.program", L"missing-task-gpg.exe" })
                           .Succeeded());
        auto manager = _manager(fixture);
        const auto start = manager.CaptureStart(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get());
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "signed output\n");
        const auto inspected = manager.InspectAndStage(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            start.StartCommit,
            start.StateDigest,
            fixture.Cancellation.Get());
        const TaskRunId run{ "d4000000-0000-4000-8000-000000000004" };
        const auto failed = manager.CommitOrRecover(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            run,
            inspected.AgentEndCommit,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(
            failed.Status == GitWorktreeCheckpointStatus::SigningFailure);
        VERIFY_IS_TRUE(failed.SigningConfigured);
        VERIFY_IS_TRUE(failed.Preserved);
        VERIFY_ARE_EQUAL(inspected.AgentEndCommit, _head(fixture, prepared));

        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"commit.gpgSign", L"false" })
                           .Succeeded());
        const auto recovered = manager.CommitOrRecover(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            run,
            inspected.AgentEndCommit,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(recovered.CommitCreated);
    }

    void GitWorktreeCheckpointTests::RecoversCommitAfterPostCommitObservationFailure()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"crash checkpoint",
            TaskId{ "c5000000-0000-4000-8000-000000000005" });
        PrepareReadyTaskWorktree(fixture, prepared);
        auto manager = _manager(fixture);
        const auto start = manager.CaptureStart(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get());
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "crash recovery\n");
        const auto inspected = manager.InspectAndStage(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            start.StartCommit,
            start.StateDigest,
            fixture.Cancellation.Get());

        GitCommandRunner runner;
        bool committed{};
        bool failedObservation{};
        GitWorktreeCheckpointManager unreliable{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                const auto isCommit = std::ranges::find(
                                          request.Arguments,
                                          L"commit") != request.Arguments.end();
                auto result = runner.Run(request);
                committed |= isCommit && result.Succeeded();
                if (committed && !failedObservation &&
                    request.Arguments == std::vector<std::wstring>{
                                             L"rev-parse", L"--verify", L"HEAD" })
                {
                    failedObservation = true;
                    GitCommandResult failure;
                    failure.Status = GitCommandStatus::TimedOut;
                    failure.SystemError = WAIT_TIMEOUT;
                    failure.Error = "injected post-commit observation failure";
                    return failure;
                }
                return result;
            }
        };
        const TaskRunId run{ "d5000000-0000-4000-8000-000000000005" };
        const auto uncertain = unreliable.CommitOrRecover(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            run,
            inspected.AgentEndCommit,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(
            uncertain.Status == GitWorktreeCheckpointStatus::TimedOut);
        VERIFY_IS_TRUE(committed);
        const auto committedHead = _head(fixture, prepared);
        VERIFY_ARE_NOT_EQUAL(inspected.AgentEndCommit, committedHead);

        const auto recovered = manager.CommitOrRecover(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            run,
            inspected.AgentEndCommit,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(
            recovered.Status == GitWorktreeCheckpointStatus::AlreadyCommitted);
        VERIFY_IS_FALSE(recovered.CommitCreated);
        VERIFY_ARE_EQUAL(committedHead, recovered.CheckpointCommit);
    }
}
