// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeGitWorktreeCheckpoint.h"
#include "RuntimeGitWorktreeTestSupport.h"
#include "GitSourceCaptureTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskCoreLocalTests::TestFixtures;
using namespace TaskGitLocalTests::TestSupport;
using namespace TaskRuntimeLocalTests::GitTestSupport;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        RuntimeGitWorktreeCheckpointContext _context(
            const uint64_t seed,
            const int64_t timestamp,
            const GitCheckpointId& checkpoint,
            const bool retry = false)
        {
            RuntimeGitWorktreeCheckpointContext context;
            context.Command = MakeId<CommandId>(110000 + seed);
            context.Event = MakeId<TaskEventId>(120000 + seed);
            context.Checkpoint = checkpoint;
            context.UnixMilliseconds = timestamp;
            context.RetryFailed = retry;
            return context;
        }

        TaskRunGitCheckpointRecord _checkpoint(
            const RuntimeReadyWorktreeFixture& fixture)
        {
            const auto aggregate = fixture.Aggregate();
            const auto found = std::ranges::find_if(
                aggregate.GitCheckpoints,
                [&](const auto& value) { return value.Run == fixture.Run.Run.Id; });
            VERIFY_IS_TRUE(found != aggregate.GitCheckpoints.end());
            return *found;
        }

        RuntimeGitWorktreeCheckpointResult _start(
            RuntimeReadyWorktreeFixture& fixture,
            RuntimeGitWorktreeCheckpointService& service,
            const GitCheckpointId& checkpoint,
            const uint64_t seed,
            const int64_t timestamp)
        {
            return service.Start(
                fixture.Task.Task.Id,
                fixture.Run.Run.Id,
                _context(seed, timestamp, checkpoint),
                fixture.Git.Cancellation.Get());
        }

        RuntimeGitWorktreeCheckpointResult _step(
            RuntimeReadyWorktreeFixture& fixture,
            RuntimeGitWorktreeCheckpointService& service,
            const GitCheckpointId& checkpoint,
            const uint64_t seed,
            const int64_t timestamp,
            const bool retry = false)
        {
            return service.Step(
                fixture.Task.Task.Id,
                fixture.Run.Run.Id,
                _context(seed, timestamp, checkpoint, retry),
                fixture.Git.Cancellation.Get());
        }

        RuntimeGitWorktreeCheckpointResult _complete(
            RuntimeReadyWorktreeFixture& fixture,
            RuntimeGitWorktreeCheckpointService& service,
            const GitCheckpointId& checkpoint,
            uint64_t seed,
            int64_t timestamp)
        {
            RuntimeGitWorktreeCheckpointResult result;
            for (size_t attempt = 0; attempt < 4; ++attempt)
            {
                result = _step(
                    fixture,
                    service,
                    checkpoint,
                    seed++,
                    timestamp++);
                VERIFY_IS_TRUE(result.Succeeded());
                if (result.Status == RuntimeGitWorktreeCheckpointStatus::Completed)
                {
                    return result;
                }
            }
            VERIFY_IS_TRUE(false);
            return result;
        }

        void _agentCommit(RuntimeReadyWorktreeFixture& fixture)
        {
            VERIFY_IS_TRUE(fixture.Git.Run(
                                      fixture.ManagedWorktree,
                                      { L"add", L"-A", L"--" })
                               .Succeeded());
            VERIFY_IS_TRUE(fixture.Git.Run(
                                      fixture.ManagedWorktree,
                                      {
                                          L"-c",
                                          L"user.name=Runtime Agent",
                                          L"-c",
                                          L"user.email=runtime-agent@example.invalid",
                                          L"-c",
                                          L"commit.gpgSign=false",
                                          L"-c",
                                          L"core.hooksPath=NUL",
                                          L"commit",
                                          L"--no-verify",
                                          L"-m",
                                          L"runtime agent commit",
                                      })
                               .Succeeded());
        }
    }

    class RuntimeGitWorktreeCheckpointTests
    {
        TEST_CLASS(RuntimeGitWorktreeCheckpointTests);

        TEST_METHOD(CheckpointsEveryTerminalOutcomeAndKeepsWorktreeReady);
        TEST_METHOD(FakeRunsPreserveAgentCommitsAndReuseOneWorktree);
        TEST_METHOD(RecoversStartInspectAndCommitAfterStoreRollbacksExactlyOnce);
        TEST_METHOD(PersistsSigningFailureBlocksNextRunAndRetriesExplicitly);
    };

    void RuntimeGitWorktreeCheckpointTests::CheckpointsEveryTerminalOutcomeAndKeepsWorktreeReady()
    {
        const std::array outcomes{
            TaskRunState::Completed,
            TaskRunState::Failed,
            TaskRunState::Cancelled,
            TaskRunState::Interrupted,
        };
        uint64_t seed{ 301 };
        for (const auto outcome : outcomes)
        {
            auto fixture = MakeReadyWorktree(seed);
            RuntimeGitWorktreeCheckpointService service{
                fixture->Store,
                fixture->Git.GitExecutable,
                fixture->RuntimeRoot
            };
            VERIFY_IS_TRUE(service.Valid());
            const auto checkpoint = MakeId<GitCheckpointId>(130000 + seed);
            VERIFY_IS_TRUE(_start(*fixture, service, checkpoint, seed, 3000).Succeeded());
            GitFixture::Write(
                fixture->ManagedWorktree / L"outcome.txt",
                "terminal outcome\n");
            TransitionRunToTerminal(*fixture, outcome, seed * 10, 3100);
            const auto completed = _complete(
                *fixture,
                service,
                checkpoint,
                seed * 20,
                4000);
            VERIFY_IS_TRUE(completed.Status == RuntimeGitWorktreeCheckpointStatus::Completed);
            const auto record = _checkpoint(*fixture);
            VERIFY_IS_TRUE(record.State == TaskGitCheckpointState::Succeeded);
            VERIFY_IS_TRUE(record.AutoCheckpointed);
            VERIFY_IS_FALSE(record.CheckpointCommit.empty());
            const auto worktree = fixture->Worktree();
            VERIFY_IS_TRUE(worktree.State == TaskGitWorktreeState::Ready);
            VERIFY_IS_TRUE(worktree.HasUndeliveredWork);
            VERIFY_IS_TRUE(fixture->Git.Run(
                                      fixture->ManagedWorktree,
                                      { L"status", L"--porcelain" })
                               .Stdout.empty());
            ++seed;
        }
    }

    void RuntimeGitWorktreeCheckpointTests::FakeRunsPreserveAgentCommitsAndReuseOneWorktree()
    {
        auto fixture = MakeReadyWorktree(310);
        RuntimeGitWorktreeCheckpointService service{
            fixture->Store,
            fixture->Git.GitExecutable,
            fixture->RuntimeRoot
        };
        const auto original = fixture->Worktree();
        const auto firstCheckpoint = MakeId<GitCheckpointId>(131000);
        VERIFY_IS_TRUE(_start(*fixture, service, firstCheckpoint, 1, 3000).Succeeded());
        GitFixture::Write(
            fixture->ManagedWorktree / L"tracked.txt",
            "agent commit\n");
        _agentCommit(*fixture);
        const auto agentCommit = SingleLine(RunCompleted(
            fixture->Git,
            fixture->ManagedWorktree,
            { L"rev-parse", L"HEAD" }));
        GitFixture::Write(
            fixture->ManagedWorktree / L"tracked.txt",
            "checkpoint remainder\n");
        TransitionRunToTerminal(*fixture, TaskRunState::Completed, 2000, 3100);
        VERIFY_IS_TRUE(_complete(
                           *fixture,
                           service,
                           firstCheckpoint,
                           10,
                           4000)
                           .Succeeded());
        VERIFY_ARE_EQUAL(
            agentCommit,
            SingleLine(RunCompleted(
                fixture->Git,
                fixture->ManagedWorktree,
                { L"rev-parse", L"HEAD^" })));

        auto aggregate = fixture->Aggregate();
        fixture->Run = MakeCreateRunCommand(
            fixture->Task,
            311,
            3000,
            aggregate.Task.Version,
            5000);
        VERIFY_IS_TRUE(fixture->Store->CreateTaskRun(fixture->Run).Succeeded());
        const auto secondCheckpoint = MakeId<GitCheckpointId>(131001);
        VERIFY_IS_TRUE(_start(*fixture, service, secondCheckpoint, 20, 5100).Succeeded());
        TransitionRunToTerminal(*fixture, TaskRunState::Completed, 4000, 5200);
        const auto noCommit = _complete(
            *fixture,
            service,
            secondCheckpoint,
            30,
            6000);
        VERIFY_IS_TRUE(noCommit.Succeeded());
        const auto second = _checkpoint(*fixture);
        VERIFY_IS_TRUE(second.State == TaskGitCheckpointState::NotNeeded);
        VERIFY_IS_FALSE(second.AutoCheckpointed);
        const auto reused = fixture->Worktree();
        VERIFY_ARE_EQUAL(original.WorktreePath, reused.WorktreePath);
        VERIFY_ARE_EQUAL(original.BranchRef, reused.BranchRef);
        VERIFY_IS_TRUE(reused.State == TaskGitWorktreeState::Ready);
    }

    void RuntimeGitWorktreeCheckpointTests::RecoversStartInspectAndCommitAfterStoreRollbacksExactlyOnce()
    {
        auto fixture = MakeReadyWorktree(320);
        RuntimeGitWorktreeCheckpointService service{
            fixture->Store,
            fixture->Git.GitExecutable,
            fixture->RuntimeRoot
        };
        const auto checkpoint = MakeId<GitCheckpointId>(132000);
        ExecuteDatabaseSql(fixture->Database.Path(), R"SQL(
CREATE TRIGGER fail_checkpoint_start
BEFORE INSERT ON task_event
WHEN NEW.type='TaskRunGitCheckpointStarted'
BEGIN SELECT RAISE(ABORT, 'injected checkpoint start failure'); END;
)SQL");
        VERIFY_IS_TRUE(
            _start(*fixture, service, checkpoint, 1, 3000).Status ==
            RuntimeGitWorktreeCheckpointStatus::StoreFailure);
        VERIFY_IS_TRUE(fixture->Aggregate().GitCheckpoints.empty());
        ExecuteDatabaseSql(
            fixture->Database.Path(),
            "DROP TRIGGER fail_checkpoint_start;");
        VERIFY_IS_TRUE(_start(*fixture, service, checkpoint, 2, 3100).Succeeded());

        GitFixture::Write(
            fixture->ManagedWorktree / L"tracked.txt",
            "rollback recovery\n");
        TransitionRunToTerminal(*fixture, TaskRunState::Interrupted, 5000, 3200);
        VERIFY_IS_TRUE(_step(*fixture, service, checkpoint, 3, 4000).Succeeded());
        VERIFY_IS_TRUE(
            _checkpoint(*fixture).State == TaskGitCheckpointState::Inspecting);
        ExecuteDatabaseSql(fixture->Database.Path(), R"SQL(
CREATE TRIGGER fail_checkpoint_advance
BEFORE INSERT ON task_event
WHEN NEW.type='TaskRunGitCheckpointAdvanced'
BEGIN SELECT RAISE(ABORT, 'injected checkpoint advance failure'); END;
)SQL");
        VERIFY_IS_TRUE(
            _step(*fixture, service, checkpoint, 4, 4100).Status ==
            RuntimeGitWorktreeCheckpointStatus::StoreFailure);
        VERIFY_IS_TRUE(
            _checkpoint(*fixture).State == TaskGitCheckpointState::Inspecting);
        ExecuteDatabaseSql(
            fixture->Database.Path(),
            "DROP TRIGGER fail_checkpoint_advance;");
        VERIFY_IS_TRUE(_step(*fixture, service, checkpoint, 5, 4200).Succeeded());
        VERIFY_IS_TRUE(
            _checkpoint(*fixture).State == TaskGitCheckpointState::Committing);

        ExecuteDatabaseSql(fixture->Database.Path(), R"SQL(
CREATE TRIGGER fail_checkpoint_commit_publish
BEFORE INSERT ON task_event
WHEN NEW.type='TaskRunGitCheckpointAdvanced'
BEGIN SELECT RAISE(ABORT, 'injected checkpoint commit publish failure'); END;
)SQL");
        VERIFY_IS_TRUE(
            _step(*fixture, service, checkpoint, 6, 4300).Status ==
            RuntimeGitWorktreeCheckpointStatus::StoreFailure);
        const auto committedHead = SingleLine(RunCompleted(
            fixture->Git,
            fixture->ManagedWorktree,
            { L"rev-parse", L"HEAD" }));
        VERIFY_IS_TRUE(
            _checkpoint(*fixture).State == TaskGitCheckpointState::Committing);
        ExecuteDatabaseSql(
            fixture->Database.Path(),
            "DROP TRIGGER fail_checkpoint_commit_publish;");
        const auto recovered = _step(*fixture, service, checkpoint, 7, 4400);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_ARE_EQUAL(committedHead, recovered.CheckpointCommit);
        VERIFY_ARE_EQUAL(
            committedHead,
            SingleLine(RunCompleted(
                fixture->Git,
                fixture->ManagedWorktree,
                { L"rev-parse", L"HEAD" })));
        VERIFY_IS_TRUE(
            _checkpoint(*fixture).State == TaskGitCheckpointState::Succeeded);
    }

    void RuntimeGitWorktreeCheckpointTests::PersistsSigningFailureBlocksNextRunAndRetriesExplicitly()
    {
        auto fixture = MakeReadyWorktree(330);
        RuntimeGitWorktreeCheckpointService service{
            fixture->Store,
            fixture->Git.GitExecutable,
            fixture->RuntimeRoot
        };
        const auto checkpoint = MakeId<GitCheckpointId>(133000);
        VERIFY_IS_TRUE(_start(*fixture, service, checkpoint, 1, 3000).Succeeded());
        GitFixture::Write(
            fixture->ManagedWorktree / L"tracked.txt",
            "signing failure\n");
        TransitionRunToTerminal(*fixture, TaskRunState::Failed, 6000, 3100);
        VERIFY_IS_TRUE(_step(*fixture, service, checkpoint, 2, 4000).Succeeded());
        VERIFY_IS_TRUE(_step(*fixture, service, checkpoint, 3, 4100).Succeeded());
        VERIFY_IS_TRUE(fixture->Git.Run(
                                  fixture->ManagedWorktree,
                                  { L"config", L"commit.gpgSign", L"true" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture->Git.Run(
                                  fixture->ManagedWorktree,
                                  { L"config", L"gpg.program", L"missing-task-gpg.exe" })
                           .Succeeded());
        const auto failed = _step(*fixture, service, checkpoint, 4, 4200);
        VERIFY_IS_TRUE(
            failed.Status == RuntimeGitWorktreeCheckpointStatus::OperationFailed);
        VERIFY_IS_TRUE(failed.FailurePersisted);
        VERIFY_ARE_EQUAL(
            std::string{ "git-checkpoint-signing-failed" },
            failed.ReasonCode);
        VERIFY_IS_TRUE(
            _checkpoint(*fixture).State == TaskGitCheckpointState::Failed);

        const auto aggregate = fixture->Aggregate();
        const auto nextRun = MakeCreateRunCommand(
            fixture->Task,
            331,
            7000,
            aggregate.Task.Version,
            5000);
        VERIFY_IS_TRUE(
            fixture->Store->CreateTaskRun(nextRun).Status ==
            TaskStoreCommandStatus::InvalidRecord);
        VERIFY_IS_TRUE(fixture->Git.Run(
                                  fixture->ManagedWorktree,
                                  { L"config", L"commit.gpgSign", L"false" })
                           .Succeeded());
        VERIFY_IS_TRUE(_step(*fixture, service, checkpoint, 5, 5100, true).Succeeded());
        const auto recovered = _step(*fixture, service, checkpoint, 6, 5200);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(
            _checkpoint(*fixture).State == TaskGitCheckpointState::Succeeded);
    }
}
