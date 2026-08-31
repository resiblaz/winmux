// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskCoreStoreTestFixtures.h"
#include "TaskGitWorktreeStatePolicy.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    namespace
    {
        CreateTaskCommand _makeWorktreeTask(
            const uint64_t taskSeed = 1,
            const uint64_t commandSeed = 1,
            const int64_t timestamp = 1000)
        {
            auto task = MakeCreateTaskCommand(
                taskSeed,
                taskSeed,
                commandSeed,
                timestamp);
            task.ExecutionContext.Mode = ExecutionMode::Worktree;
            task.ExecutionContext.SourceRoot =
                "C:\\src\\transition-" + std::to_string(taskSeed);
            task.ExecutionContext.RepositoryCommonDirectory =
                task.ExecutionContext.SourceRoot + "\\.git";
            task.ExecutionContext.WorktreePath =
                "C:\\task-worktrees\\" + task.Task.Id.Value();
            task.ExecutionContext.RuntimeRoot = task.ExecutionContext.WorktreePath;
            task.ExecutionContext.Branch =
                "refs/heads/wt-task/" + task.Task.Id.Value();
            return task;
        }

        void _initializeWorktree(
            SqliteTaskStore& store,
            const CreateTaskCommand& task)
        {
            VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
            VERIFY_IS_TRUE(store.CreateTaskRun(
                                    MakeCreateRunCommand(task, 1, 2, 1, 1100))
                               .Succeeded());
            VERIFY_IS_TRUE(store.InitializeTaskGitWorktree(
                                    MakeInitializeGitWorktreeCommand(
                                        task,
                                        3,
                                        2,
                                        1200))
                               .Succeeded());
        }

        void _executeSql(
            const std::filesystem::path& path,
            const std::string_view sql)
        {
            auto database = OpenRawDatabase(path);
            VERIFY_ARE_EQUAL(
                SQLITE_OK,
                sqlite3_exec(database, sql.data(), nullptr, nullptr, nullptr));
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
        }
    }

    class TaskCoreGitWorktreeTransitionTests
    {
        TEST_CLASS(TaskCoreGitWorktreeTransitionTests);

        TEST_METHOD(SetupPolicyAllowsOnlyForwardFailureAndRecoveryEdges);
        TEST_METHOD(AdvancesSetupEvidenceAtomicallyToReady);
        TEST_METHOD(RejectsStaleIdentityEvidenceAndInvalidTransitions);
        TEST_METHOD(FailureRetryAndEventRollbackRemainDurable);
    };

    void TaskCoreGitWorktreeTransitionTests::SetupPolicyAllowsOnlyForwardFailureAndRecoveryEdges()
    {
        const std::array forward{
            std::pair{ TaskGitWorktreeState::Planning, TaskGitWorktreeState::CapturingSource },
            std::pair{ TaskGitWorktreeState::CapturingSource, TaskGitWorktreeState::Provisioning },
            std::pair{ TaskGitWorktreeState::Provisioning, TaskGitWorktreeState::RestoringSource },
            std::pair{ TaskGitWorktreeState::RestoringSource, TaskGitWorktreeState::CreatingBaseline },
            std::pair{ TaskGitWorktreeState::CreatingBaseline, TaskGitWorktreeState::Ready },
        };
        for (const auto [current, requested] : forward)
        {
            VERIFY_IS_TRUE(
                EvaluateTaskGitWorktreeSetupTransition(current, requested).Allowed);
            VERIFY_IS_TRUE(
                EvaluateTaskGitWorktreeSetupTransition(
                    current,
                    TaskGitWorktreeState::Failed)
                    .Allowed);
        }

        for (const auto retry : {
                 TaskGitWorktreeState::CapturingSource,
                 TaskGitWorktreeState::Provisioning,
                 TaskGitWorktreeState::RestoringSource,
                 TaskGitWorktreeState::CreatingBaseline,
             })
        {
            VERIFY_IS_TRUE(
                EvaluateTaskGitWorktreeSetupTransition(
                    TaskGitWorktreeState::Failed,
                    retry)
                    .Allowed);
        }
        VERIFY_IS_FALSE(EvaluateTaskGitWorktreeSetupTransition(
                            TaskGitWorktreeState::Planning,
                            TaskGitWorktreeState::Provisioning)
                            .Allowed);
        VERIFY_IS_FALSE(EvaluateTaskGitWorktreeSetupTransition(
                            TaskGitWorktreeState::Provisioning,
                            TaskGitWorktreeState::CapturingSource)
                            .Allowed);
        VERIFY_IS_FALSE(EvaluateTaskGitWorktreeSetupTransition(
                            TaskGitWorktreeState::Ready,
                            TaskGitWorktreeState::Failed)
                            .Allowed);
        VERIFY_IS_FALSE(EvaluateTaskGitWorktreeSetupTransition(
                            TaskGitWorktreeState::Finalizing,
                            TaskGitWorktreeState::Failed)
                            .Allowed);
    }

    void TaskCoreGitWorktreeTransitionTests::AdvancesSetupEvidenceAtomicallyToReady()
    {
        TemporaryDatabasePath path;
        const auto task = _makeWorktreeTask();
        SqliteTaskStore store{ path.Path() };
        _initializeWorktree(store, task);
        const std::string snapshot(64, 'c');
        const std::string baseline(40, 'd');
        const auto preserved = task.ExecutionContext.WorktreePath;

        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(
                                MakeAdvanceGitWorktreeCommand(
                                    task,
                                    4,
                                    3,
                                    1,
                                    1300,
                                    TaskGitWorktreeState::CapturingSource))
                           .Succeeded());
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(
                                MakeAdvanceGitWorktreeCommand(
                                    task,
                                    5,
                                    4,
                                    2,
                                    1400,
                                    TaskGitWorktreeState::Provisioning,
                                    snapshot))
                           .Succeeded());
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(
                                MakeAdvanceGitWorktreeCommand(
                                    task,
                                    6,
                                    5,
                                    3,
                                    1500,
                                    TaskGitWorktreeState::RestoringSource,
                                    snapshot,
                                    {},
                                    true,
                                    preserved))
                           .Succeeded());
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(
                                MakeAdvanceGitWorktreeCommand(
                                    task,
                                    7,
                                    6,
                                    4,
                                    1600,
                                    TaskGitWorktreeState::CreatingBaseline,
                                    snapshot,
                                    {},
                                    true,
                                    preserved))
                           .Succeeded());
        const auto ready = store.AdvanceTaskGitWorktree(
            MakeAdvanceGitWorktreeCommand(
                task,
                8,
                7,
                5,
                1700,
                TaskGitWorktreeState::Ready,
                snapshot,
                baseline));
        VERIFY_IS_TRUE(ready.Succeeded());
        VERIFY_ARE_EQUAL(8ull, ready.CommittedTaskVersion);
        VERIFY_ARE_EQUAL(8ull, ready.LastEventSequence);

        const auto loaded = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(loaded.has_value());
        VERIFY_IS_TRUE(loaded->GitWorktree.has_value());
        VERIFY_ARE_EQUAL(6ull, loaded->GitWorktree->Revision);
        VERIFY_IS_TRUE(
            loaded->GitWorktree->State == TaskGitWorktreeState::Ready);
        VERIFY_ARE_EQUAL(snapshot, loaded->GitWorktree->SnapshotDigest);
        VERIFY_ARE_EQUAL(baseline, loaded->GitWorktree->BaselineCommit);
        VERIFY_IS_FALSE(loaded->GitWorktree->HasUndeliveredWork);
        VERIFY_IS_FALSE(loaded->GitWorktree->CleanupEligible);
        VERIFY_IS_TRUE(loaded->GitWorktree->PreservedPath.empty());
        VERIFY_ARE_EQUAL(8ull, loaded->Task.Version);
        const auto events = store.ListEvents(task.Task.Id, 0, 16);
        VERIFY_ARE_EQUAL(size_t{ 8 }, events.size());
        VERIFY_ARE_EQUAL(
            std::string{ "TaskGitWorktreeAdvanced" },
            events.back().Type);
    }

    void TaskCoreGitWorktreeTransitionTests::RejectsStaleIdentityEvidenceAndInvalidTransitions()
    {
        TemporaryDatabasePath path;
        const auto task = _makeWorktreeTask();
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(
                                MakeCreateRunCommand(task, 1, 2, 1, 1100))
                           .Succeeded());
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktree(MakeAdvanceGitWorktreeCommand(
                    task,
                    3,
                    2,
                    1,
                    1200,
                    TaskGitWorktreeState::CapturingSource))
                    .Status == TaskStoreCommandStatus::NotFound);
        VERIFY_IS_TRUE(store.InitializeTaskGitWorktree(
                                MakeInitializeGitWorktreeCommand(task, 4, 2, 1200))
                           .Succeeded());

        const std::string snapshot(64, 'c');
        auto skipped = MakeAdvanceGitWorktreeCommand(
            task,
            5,
            3,
            1,
            1300,
            TaskGitWorktreeState::Provisioning,
            snapshot);
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktree(skipped).Status ==
            TaskStoreCommandStatus::InvalidTransition);

        auto capturing = MakeAdvanceGitWorktreeCommand(
            task,
            6,
            3,
            1,
            1300,
            TaskGitWorktreeState::CapturingSource);
        const auto applied = store.AdvanceTaskGitWorktree(capturing);
        VERIFY_IS_TRUE(applied.Succeeded());
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktree(capturing).Status ==
            TaskStoreCommandStatus::Replayed);
        auto receiptConflict = capturing;
        receiptConflict.Metadata.RequestDigest.assign(64, 'f');
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktree(receiptConflict).Status ==
            TaskStoreCommandStatus::IdempotencyConflict);

        auto staleRevision = MakeAdvanceGitWorktreeCommand(
            task,
            7,
            4,
            1,
            1400,
            TaskGitWorktreeState::Provisioning,
            snapshot);
        const auto stale = store.AdvanceTaskGitWorktree(staleRevision);
        VERIFY_IS_TRUE(stale.Status == TaskStoreCommandStatus::VersionConflict);
        VERIFY_IS_TRUE(
            stale.Transition == TransitionError::GitWorktreeRevisionMismatch);

        auto wrongContext = staleRevision;
        wrongContext.ExpectedWorktreeRevision = 2;
        wrongContext.ExecutionContext = MakeId<ExecutionContextId>(9999);
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktree(wrongContext).Status ==
            TaskStoreCommandStatus::InvalidRecord);

        auto validProvision = staleRevision;
        validProvision.ExpectedWorktreeRevision = 2;
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(validProvision).Succeeded());
        auto wrongPath = MakeAdvanceGitWorktreeCommand(
            task,
            8,
            5,
            3,
            1500,
            TaskGitWorktreeState::RestoringSource,
            snapshot,
            {},
            true,
            "C:\\different");
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktree(wrongPath).Status ==
            TaskStoreCommandStatus::InvalidRecord);

        auto snapshotDrift = wrongPath;
        snapshotDrift.PreservedPath = task.ExecutionContext.WorktreePath;
        snapshotDrift.SnapshotDigest.assign(64, 'd');
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktree(snapshotDrift).Status ==
            TaskStoreCommandStatus::InvalidRecord);

        auto missingFailureReason = MakeAdvanceGitWorktreeCommand(
            task,
            9,
            5,
            3,
            1500,
            TaskGitWorktreeState::Failed,
            snapshot);
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktree(missingFailureReason).Status ==
            TaskStoreCommandStatus::InvalidCommand);
    }

    void TaskCoreGitWorktreeTransitionTests::FailureRetryAndEventRollbackRemainDurable()
    {
        TemporaryDatabasePath path;
        const auto task = _makeWorktreeTask();
        SqliteTaskStore store{ path.Path() };
        _initializeWorktree(store, task);
        const std::string snapshot(64, 'c');
        const auto preserved = task.ExecutionContext.WorktreePath;
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(
                                MakeAdvanceGitWorktreeCommand(
                                    task,
                                    4,
                                    3,
                                    1,
                                    1300,
                                    TaskGitWorktreeState::CapturingSource))
                           .Succeeded());
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(
                                MakeAdvanceGitWorktreeCommand(
                                    task,
                                    5,
                                    4,
                                    2,
                                    1400,
                                    TaskGitWorktreeState::Provisioning,
                                    snapshot))
                           .Succeeded());
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(
                                MakeAdvanceGitWorktreeCommand(
                                    task,
                                    6,
                                    5,
                                    3,
                                    1500,
                                    TaskGitWorktreeState::RestoringSource,
                                    snapshot,
                                    {},
                                    true,
                                    preserved))
                           .Succeeded());
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(
                                MakeAdvanceGitWorktreeCommand(
                                    task,
                                    7,
                                    6,
                                    4,
                                    1600,
                                    TaskGitWorktreeState::Failed,
                                    snapshot,
                                    {},
                                    true,
                                    preserved,
                                    "copy-failed"))
                           .Succeeded());
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(
                                MakeAdvanceGitWorktreeCommand(
                                    task,
                                    8,
                                    7,
                                    5,
                                    1700,
                                    TaskGitWorktreeState::RestoringSource,
                                    snapshot,
                                    {},
                                    true,
                                    preserved))
                           .Succeeded());

        _executeSql(path.Path(), R"SQL(
CREATE TRIGGER fail_git_advance_event
BEFORE INSERT ON task_event
WHEN NEW.type='TaskGitWorktreeAdvanced'
BEGIN SELECT RAISE(ABORT, 'injected git event failure'); END;
)SQL");
        const auto creating = MakeAdvanceGitWorktreeCommand(
            task,
            9,
            8,
            6,
            1800,
            TaskGitWorktreeState::CreatingBaseline,
            snapshot,
            {},
            true,
            preserved);
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktree(creating).Status ==
            TaskStoreCommandStatus::StorageError);
        auto loaded = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(loaded.has_value());
        VERIFY_ARE_EQUAL(6ull, loaded->GitWorktree->Revision);
        VERIFY_IS_TRUE(
            loaded->GitWorktree->State == TaskGitWorktreeState::RestoringSource);
        VERIFY_ARE_EQUAL(8ull, loaded->Task.Version);

        _executeSql(path.Path(), "DROP TRIGGER fail_git_advance_event;");
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(creating).Succeeded());
        loaded = store.LoadTask(task.Task.Id);
        VERIFY_ARE_EQUAL(7ull, loaded->GitWorktree->Revision);
        VERIFY_IS_TRUE(
            loaded->GitWorktree->State == TaskGitWorktreeState::CreatingBaseline);
        VERIFY_IS_TRUE(loaded->GitWorktree->FailureReasonCode.empty());
    }
}
