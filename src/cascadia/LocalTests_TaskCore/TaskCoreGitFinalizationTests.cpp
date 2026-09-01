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
        struct FinalizationFixture
        {
            CreateTaskCommand Task;
            CreateTaskRunCommand Run;
            uint64_t Version{};
            uint64_t WorktreeRevision{};
        };

        CreateTaskCommand _task(const uint64_t seed)
        {
            auto task = MakeCreateTaskCommand(seed, seed, 1, 1000);
            task.ExecutionContext.Mode = ExecutionMode::Worktree;
            task.ExecutionContext.SourceRoot =
                "C:\\src\\finalization-" + std::to_string(seed);
            task.ExecutionContext.RepositoryCommonDirectory =
                task.ExecutionContext.SourceRoot + "\\.git";
            task.ExecutionContext.WorktreePath =
                "C:\\task-worktrees\\" + task.Task.Id.Value();
            task.ExecutionContext.RuntimeRoot = task.ExecutionContext.WorktreePath;
            task.ExecutionContext.Branch =
                "refs/heads/wt-task/" + task.Task.Id.Value();
            return task;
        }

        FinalizationFixture _ready(SqliteTaskStore& store, const uint64_t seed)
        {
            FinalizationFixture fixture;
            fixture.Task = _task(seed);
            fixture.Run = MakeCreateRunCommand(fixture.Task, seed, 2, 1, 1100);
            VERIFY_IS_TRUE(store.CreateTask(fixture.Task).Succeeded());
            VERIFY_IS_TRUE(store.CreateTaskRun(fixture.Run).Succeeded());
            VERIFY_IS_TRUE(store.InitializeTaskGitWorktree(
                                    MakeInitializeGitWorktreeCommand(
                                        fixture.Task,
                                        3,
                                        2,
                                        1200))
                               .Succeeded());
            const std::string snapshot(64, 'c');
            fixture.Version = 3;
            fixture.WorktreeRevision = 1;
            for (const auto state : {
                     TaskGitWorktreeState::CapturingSource,
                     TaskGitWorktreeState::Provisioning,
                     TaskGitWorktreeState::RestoringSource,
                     TaskGitWorktreeState::CreatingBaseline,
                     TaskGitWorktreeState::Ready,
                 })
            {
                const auto reconstructed =
                    state == TaskGitWorktreeState::RestoringSource ||
                    state == TaskGitWorktreeState::CreatingBaseline;
                auto command = MakeAdvanceGitWorktreeCommand(
                    fixture.Task,
                    10 + fixture.Version,
                    fixture.Version,
                    fixture.WorktreeRevision,
                    1300 + static_cast<int64_t>(fixture.Version) * 10,
                    state,
                    state == TaskGitWorktreeState::CapturingSource ?
                        std::string{} : snapshot,
                    state == TaskGitWorktreeState::Ready ?
                        std::string(40, 'd') : std::string{},
                    reconstructed,
                    reconstructed ? fixture.Task.ExecutionContext.WorktreePath :
                                    std::string{});
                VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(command).Succeeded());
                ++fixture.Version;
                ++fixture.WorktreeRevision;
            }
            return fixture;
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

    class TaskCoreGitFinalizationTests
    {
        TEST_CLASS(TaskCoreGitFinalizationTests);

        TEST_METHOD(FinalizationPolicyAllowsOnlyPreservingRetryAndCompletion);
        TEST_METHOD(GuardsActiveRunPersistsFailureRetriesAndEnablesDelete);
        TEST_METHOD(IncompleteCheckpointBlocksFinalizationUntilDelivered);
    };

    void TaskCoreGitFinalizationTests::FinalizationPolicyAllowsOnlyPreservingRetryAndCompletion()
    {
        VERIFY_IS_TRUE(EvaluateTaskGitWorktreeFinalizationTransition(
                           TaskGitWorktreeState::Ready,
                           TaskGitWorktreeState::Finalizing)
                           .Allowed);
        VERIFY_IS_TRUE(EvaluateTaskGitWorktreeFinalizationTransition(
                           TaskGitWorktreeState::Finalizing,
                           TaskGitWorktreeState::Preserved)
                           .Allowed);
        VERIFY_IS_TRUE(EvaluateTaskGitWorktreeFinalizationTransition(
                           TaskGitWorktreeState::Preserved,
                           TaskGitWorktreeState::Finalizing)
                           .Allowed);
        VERIFY_IS_TRUE(EvaluateTaskGitWorktreeFinalizationTransition(
                           TaskGitWorktreeState::Finalizing,
                           TaskGitWorktreeState::Finalized)
                           .Allowed);
        VERIFY_IS_FALSE(EvaluateTaskGitWorktreeFinalizationTransition(
                            TaskGitWorktreeState::Ready,
                            TaskGitWorktreeState::Finalized)
                            .Allowed);
        VERIFY_IS_FALSE(EvaluateTaskGitWorktreeFinalizationTransition(
                            TaskGitWorktreeState::Finalized,
                            TaskGitWorktreeState::Finalizing)
                            .Allowed);
    }

    void TaskCoreGitFinalizationTests::GuardsActiveRunPersistsFailureRetriesAndEnablesDelete()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        auto fixture = _ready(store, 401);
        auto start = MakeAdvanceGitWorktreeFinalizationCommand(
            fixture.Task,
            100,
            fixture.Version,
            fixture.WorktreeRevision,
            2000,
            TaskGitWorktreeState::Finalizing,
            false);
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktreeFinalization(start).Status ==
            TaskStoreCommandStatus::ActiveRunExists);
        VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                MakeRunStateCommand(
                                    fixture.Run,
                                    101,
                                    fixture.Version,
                                    2100,
                                    TaskRunState::Cancelled,
                                    {},
                                    "cancelled-before-cleanup"))
                           .Succeeded());
        ++fixture.Version;
        start.Metadata = MakeMetadata(102, fixture.Version, 2200);

        _executeSql(path.Path(), R"SQL(
CREATE TRIGGER fail_finalization_event
BEFORE INSERT ON task_event
WHEN NEW.type='TaskGitWorktreeFinalizationAdvanced'
BEGIN SELECT RAISE(ABORT, 'injected finalization event failure'); END;
)SQL");
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktreeFinalization(start).Status ==
            TaskStoreCommandStatus::StorageError);
        auto snapshot = store.LoadTask(fixture.Task.Task.Id);
        VERIFY_IS_TRUE(snapshot->GitWorktree->State == TaskGitWorktreeState::Ready);
        VERIFY_ARE_EQUAL(fixture.WorktreeRevision, snapshot->GitWorktree->Revision);
        _executeSql(path.Path(), "DROP TRIGGER fail_finalization_event;");
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktreeFinalization(start).Succeeded());
        ++fixture.Version;
        ++fixture.WorktreeRevision;

        auto preserved = MakeAdvanceGitWorktreeFinalizationCommand(
            fixture.Task,
            103,
            fixture.Version,
            fixture.WorktreeRevision,
            2300,
            TaskGitWorktreeState::Preserved,
            true,
            false,
            fixture.Task.ExecutionContext.WorktreePath,
            "git-finalize-signing-failed");
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktreeFinalization(preserved).Succeeded());
        ++fixture.Version;
        ++fixture.WorktreeRevision;
        const auto blockedRun = MakeCreateRunCommand(
            fixture.Task,
            402,
            104,
            fixture.Version,
            2400);
        VERIFY_IS_TRUE(
            store.CreateTaskRun(blockedRun).Status ==
            TaskStoreCommandStatus::InvalidRecord);

        auto retry = MakeAdvanceGitWorktreeFinalizationCommand(
            fixture.Task,
            105,
            fixture.Version,
            fixture.WorktreeRevision,
            2500,
            TaskGitWorktreeState::Finalizing,
            true);
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktreeFinalization(retry).Succeeded());
        ++fixture.Version;
        ++fixture.WorktreeRevision;
        auto finalized = MakeAdvanceGitWorktreeFinalizationCommand(
            fixture.Task,
            106,
            fixture.Version,
            fixture.WorktreeRevision,
            2600,
            TaskGitWorktreeState::Finalized,
            false,
            true);
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktreeFinalization(finalized).Succeeded());
        ++fixture.Version;
        ++fixture.WorktreeRevision;
        snapshot = store.LoadTask(fixture.Task.Task.Id);
        VERIFY_IS_TRUE(snapshot->GitWorktree->State == TaskGitWorktreeState::Finalized);
        VERIFY_IS_FALSE(snapshot->GitWorktree->HasUndeliveredWork);
        VERIFY_IS_TRUE(snapshot->GitWorktree->CleanupEligible);

        auto intent = MakeStartIntentCommand(
            fixture.Task,
            1,
            107,
            fixture.Version,
            2700,
            TaskIntentKind::Delete);
        VERIFY_IS_TRUE(store.StartTaskIntent(intent).Succeeded());
        ++fixture.Version;
        auto deleting = MakeAdvanceIntentCommand(
            intent,
            108,
            fixture.Version,
            1,
            2800,
            TaskIntentStage::DeletingAggregate);
        VERIFY_IS_TRUE(store.AdvanceTaskIntent(deleting).Succeeded());
        ++fixture.Version;
        const auto removed = store.DeleteTask(MakeDeleteTaskCommand(
            intent,
            109,
            fixture.Version,
            2,
            2900));
        VERIFY_IS_TRUE(removed.Succeeded());
        VERIFY_IS_FALSE(store.LoadTask(fixture.Task.Task.Id).has_value());
        finalized.Metadata = MakeMetadata(110, fixture.Version, 3000);
        finalized.ExpectedWorktreeRevision = fixture.WorktreeRevision;
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktreeFinalization(finalized).Status ==
            TaskStoreCommandStatus::Tombstoned);
    }

    void TaskCoreGitFinalizationTests::IncompleteCheckpointBlocksFinalizationUntilDelivered()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        auto fixture = _ready(store, 402);
        auto begin = MakeBeginGitCheckpointCommand(
            fixture.Run,
            10,
            200,
            fixture.Version,
            2000);
        VERIFY_IS_TRUE(store.BeginTaskRunGitCheckpoint(begin).Succeeded());
        ++fixture.Version;
        VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                MakeRunStateCommand(
                                    fixture.Run,
                                    201,
                                    fixture.Version,
                                    2100,
                                    TaskRunState::Interrupted,
                                    {},
                                    "interrupted"))
                           .Succeeded());
        ++fixture.Version;
        auto finalizing = MakeAdvanceGitWorktreeFinalizationCommand(
            fixture.Task,
            202,
            fixture.Version,
            fixture.WorktreeRevision,
            2200,
            TaskGitWorktreeState::Finalizing,
            false);
        VERIFY_IS_TRUE(
            store.AdvanceTaskGitWorktreeFinalization(finalizing).Status ==
            TaskStoreCommandStatus::InvalidTransition);

        auto inspecting = MakeAdvanceGitCheckpointCommand(
            begin,
            203,
            fixture.Version,
            1,
            fixture.WorktreeRevision,
            2300,
            TaskGitCheckpointState::Inspecting);
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(inspecting).Succeeded());
        ++fixture.Version;
        ++fixture.WorktreeRevision;
        auto noCommit = MakeAdvanceGitCheckpointCommand(
            begin,
            204,
            fixture.Version,
            2,
            fixture.WorktreeRevision,
            2400,
            TaskGitCheckpointState::NotNeeded);
        noCommit.EndCommit.assign(40, 'd');
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(noCommit).Succeeded());
        ++fixture.Version;
        ++fixture.WorktreeRevision;
        finalizing.Metadata = MakeMetadata(205, fixture.Version, 2500);
        finalizing.ExpectedWorktreeRevision = fixture.WorktreeRevision;
        VERIFY_IS_TRUE(store.AdvanceTaskGitWorktreeFinalization(finalizing).Succeeded());
    }
}
