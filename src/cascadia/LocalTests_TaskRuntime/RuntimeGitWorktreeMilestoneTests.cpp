// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeGitWorktreeCheckpoint.h"
#include "RuntimeGitWorktreeFinalize.h"
#include "RuntimeGitWorktreeTestSupport.h"

#include "GitSourceCommandOutput.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskCoreLocalTests::TestFixtures;
using namespace TaskGitLocalTests::TestSupport;
using namespace TaskRuntimeLocalTests::GitTestSupport;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        void _reopen(RuntimeReadyWorktreeFixture& fixture)
        {
            fixture.Store.reset();
            fixture.Store = std::make_shared<SqliteTaskStore>(
                fixture.Database.Path());
            VERIFY_IS_TRUE(fixture.Store->IsReady());
        }

        RuntimeGitWorktreeCheckpointContext _checkpointContext(
            const uint64_t seed,
            const int64_t timestamp,
            const GitCheckpointId& checkpoint)
        {
            RuntimeGitWorktreeCheckpointContext context;
            context.Command = MakeId<CommandId>(160000 + seed);
            context.Event = MakeId<TaskEventId>(170000 + seed);
            context.Checkpoint = checkpoint;
            context.UnixMilliseconds = timestamp;
            return context;
        }

        RuntimeGitWorktreeFinalizeContext _finalizeContext(
            const uint64_t seed,
            const int64_t timestamp)
        {
            RuntimeGitWorktreeFinalizeContext context;
            context.Command = MakeId<CommandId>(180000 + seed);
            context.Event = MakeId<TaskEventId>(190000 + seed);
            context.UnixMilliseconds = timestamp;
            return context;
        }

        RuntimeGitWorktreeCheckpointResult _checkpointStepAfterRestart(
            RuntimeReadyWorktreeFixture& fixture,
            const GitCheckpointId& checkpoint,
            const uint64_t seed,
            const int64_t timestamp)
        {
            RuntimeGitWorktreeCheckpointService service{
                fixture.Store,
                fixture.Git.GitExecutable,
                fixture.RuntimeRoot
            };
            VERIFY_IS_TRUE(service.Valid());
            return service.Step(
                fixture.Task.Task.Id,
                fixture.Run.Run.Id,
                _checkpointContext(seed, timestamp, checkpoint),
                fixture.Git.Cancellation.Get());
        }

        RuntimeGitWorktreeFinalizeResult _finalizeStepAfterRestart(
            RuntimeReadyWorktreeFixture& fixture,
            const uint64_t seed,
            const int64_t timestamp)
        {
            RuntimeGitWorktreeFinalizeService service{
                fixture.Store,
                fixture.Git.GitExecutable,
                fixture.RuntimeRoot
            };
            VERIFY_IS_TRUE(service.Valid());
            return service.Step(
                fixture.Task.Task.Id,
                _finalizeContext(seed, timestamp),
                fixture.Git.Cancellation.Get());
        }
    }

    class RuntimeGitWorktreeMilestoneTests
    {
        TEST_CLASS(RuntimeGitWorktreeMilestoneTests);

        TEST_METHOD(RepeatedSqliteAndServiceRestartsResumeCheckpointAndFinalize);
    };

    void RuntimeGitWorktreeMilestoneTests::RepeatedSqliteAndServiceRestartsResumeCheckpointAndFinalize()
    {
        DWORD handlesBefore{};
        VERIFY_IS_TRUE(GetProcessHandleCount(
                           GetCurrentProcess(),
                           &handlesBefore) != FALSE);
        constexpr size_t rounds{ 3 };
        for (size_t round = 0; round < rounds; ++round)
        {
            const auto taskSeed = 601 + static_cast<uint64_t>(round);
            auto fixture = MakeReadyWorktree(taskSeed);
            const auto checkpoint = MakeId<GitCheckpointId>(200000 + taskSeed);
            {
                RuntimeGitWorktreeCheckpointService service{
                    fixture->Store,
                    fixture->Git.GitExecutable,
                    fixture->RuntimeRoot
                };
                VERIFY_IS_TRUE(service.Valid());
                const auto started = service.Start(
                    fixture->Task.Task.Id,
                    fixture->Run.Run.Id,
                    _checkpointContext(taskSeed * 10, 3000, checkpoint),
                    fixture->Git.Cancellation.Get());
                VERIFY_IS_TRUE(started.Succeeded());
            }
            _reopen(*fixture);
            GitFixture::Write(
                fixture->ManagedWorktree / L"milestone.txt",
                "restart-safe output " + std::to_string(round) + "\n");
            TransitionRunToTerminal(
                *fixture,
                round % 2 == 0 ? TaskRunState::Completed :
                                 TaskRunState::Interrupted,
                taskSeed * 100,
                3100);
            _reopen(*fixture);

            auto checkpointResult = _checkpointStepAfterRestart(
                *fixture,
                checkpoint,
                taskSeed * 10 + 1,
                4000);
            VERIFY_IS_TRUE(checkpointResult.Succeeded());
            _reopen(*fixture);
            const auto recovery = fixture->Store->LoadRecoverySnapshot(32);
            VERIFY_IS_TRUE(recovery.Succeeded);
            VERIFY_IS_TRUE(std::ranges::any_of(
                recovery.Snapshot.RecoverableGitCheckpoints,
                [&](const auto& value) {
                    return value.Id == checkpoint &&
                           value.State == TaskGitCheckpointState::Inspecting;
                }));

            checkpointResult = _checkpointStepAfterRestart(
                *fixture,
                checkpoint,
                taskSeed * 10 + 2,
                4100);
            VERIFY_IS_TRUE(checkpointResult.Succeeded());
            _reopen(*fixture);
            checkpointResult = _checkpointStepAfterRestart(
                *fixture,
                checkpoint,
                taskSeed * 10 + 3,
                4200);
            VERIFY_IS_TRUE(checkpointResult.Succeeded());
            VERIFY_IS_TRUE(
                checkpointResult.Status ==
                RuntimeGitWorktreeCheckpointStatus::Completed);
            _reopen(*fixture);

            auto finalizeResult = _finalizeStepAfterRestart(
                *fixture,
                taskSeed * 10 + 4,
                5000);
            VERIFY_IS_TRUE(finalizeResult.Succeeded());
            VERIFY_IS_TRUE(
                fixture->Worktree().State == TaskGitWorktreeState::Finalizing);
            _reopen(*fixture);
            finalizeResult = _finalizeStepAfterRestart(
                *fixture,
                taskSeed * 10 + 5,
                5100);
            VERIFY_IS_TRUE(finalizeResult.Succeeded());
            VERIFY_IS_TRUE(finalizeResult.BranchRetained);
            VERIFY_IS_TRUE(finalizeResult.WorktreeRemoved);
            _reopen(*fixture);
            const auto aggregate = fixture->Aggregate();
            VERIFY_IS_TRUE(
                aggregate.GitWorktree->State == TaskGitWorktreeState::Finalized);
            VERIFY_IS_FALSE(std::filesystem::exists(fixture->ManagedWorktree));
            const auto finalRecovery = fixture->Store->LoadRecoverySnapshot(32);
            VERIFY_IS_TRUE(finalRecovery.Succeeded);
            VERIFY_IS_FALSE(std::ranges::any_of(
                finalRecovery.Snapshot.RecoverableGitWorktrees,
                [&](const auto& value) { return value.Task == fixture->Task.Task.Id; }));
            VERIFY_IS_FALSE(std::ranges::any_of(
                finalRecovery.Snapshot.RecoverableGitCheckpoints,
                [&](const auto& value) { return value.Id == checkpoint; }));
        }
        DWORD handlesAfter{};
        VERIFY_IS_TRUE(GetProcessHandleCount(
                           GetCurrentProcess(),
                           &handlesAfter) != FALSE);
        VERIFY_IS_LESS_THAN_OR_EQUAL(handlesAfter, handlesBefore + 8);
    }
}
