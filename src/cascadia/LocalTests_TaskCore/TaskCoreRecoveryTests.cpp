// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskCoreStoreTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    class TaskCoreRecoveryTests
    {
        TEST_CLASS(TaskCoreRecoveryTests);

        TEST_METHOD(IntentProgressManualHoldAndResumePersistAcrossReopen);
        TEST_METHOD(RecoverySnapshotReturnsBoundedPendingResources);
        TEST_METHOD(CompletedIntentCanBeReplacedByNewLifecycleWork);
        TEST_METHOD(IntentKindRejectsUnsupportedStageWithoutMutation);
        TEST_METHOD(IncompleteCleanupBlocksRunAndStatusWithoutManualHold);
    };

    void TaskCoreRecoveryTests::IntentProgressManualHoldAndResumePersistAcrossReopen()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto closeIntent = MakeStartIntentCommand(task, 1, 2, 1, 1100, TaskIntentKind::Close);
        {
            SqliteTaskStore store{ path.Path() };
            VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
            const auto started = store.StartTaskIntent(closeIntent);
            VERIFY_IS_TRUE(started.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(2ull, started.CommittedTaskVersion);

            const auto heldSnapshot = store.LoadTask(task.Task.Id);
            VERIFY_IS_TRUE(heldSnapshot.has_value());
            VERIFY_IS_TRUE(heldSnapshot->Task.ManualHold);
            VERIFY_IS_TRUE(heldSnapshot->Intent.has_value());
            VERIFY_IS_TRUE(heldSnapshot->Intent->Stage == TaskIntentStage::Requested);
            VERIFY_ARE_EQUAL(1ull, heldSnapshot->Intent->Revision);

            const auto blockedRun = store.CreateTaskRun(MakeCreateRunCommand(task, 1, 3, 2, 1150));
            VERIFY_IS_TRUE(blockedRun.Status == TaskStoreCommandStatus::InvalidTransition);
            VERIFY_IS_TRUE(blockedRun.Transition == TransitionError::ManualHoldActive);

            const auto interrupting = store.AdvanceTaskIntent(
                MakeAdvanceIntentCommand(closeIntent, 4, 2, 1, 1200, TaskIntentStage::Interrupting));
            VERIFY_IS_TRUE(interrupting.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(3ull, interrupting.CommittedTaskVersion);

            const auto stale = store.AdvanceTaskIntent(
                MakeAdvanceIntentCommand(closeIntent, 5, 3, 1, 1250, TaskIntentStage::WaitingForProvider));
            VERIFY_IS_TRUE(stale.Status == TaskStoreCommandStatus::VersionConflict);
            VERIFY_IS_TRUE(stale.Transition == TransitionError::IntentRevisionMismatch);

            const auto waiting = store.AdvanceTaskIntent(
                MakeAdvanceIntentCommand(closeIntent, 6, 3, 2, 1300, TaskIntentStage::WaitingForProvider));
            VERIFY_IS_TRUE(waiting.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(4ull, waiting.CommittedTaskVersion);

            const auto earlyRelease = store.ChangeTaskManualHold(
                MakeManualHoldCommand(task, 7, 4, 1350, false));
            VERIFY_IS_TRUE(earlyRelease.Status == TaskStoreCommandStatus::InvalidTransition);
            VERIFY_IS_TRUE(earlyRelease.Transition == TransitionError::ActiveIntentConflict);

            const auto completed = store.AdvanceTaskIntent(
                MakeAdvanceIntentCommand(closeIntent, 8, 4, 3, 1400, TaskIntentStage::Completed));
            VERIFY_IS_TRUE(completed.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(5ull, completed.CommittedTaskVersion);

            const auto released = store.ChangeTaskManualHold(
                MakeManualHoldCommand(task, 9, 5, 1500, false));
            VERIFY_IS_TRUE(released.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(6ull, released.CommittedTaskVersion);

            const auto run = store.CreateTaskRun(MakeCreateRunCommand(task, 1, 10, 6, 1600));
            VERIFY_IS_TRUE(run.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(7ull, run.CommittedTaskVersion);
        }

        SqliteTaskStore reopened{ path.Path() };
        const auto restored = reopened.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(restored.has_value());
        VERIFY_IS_FALSE(restored->Task.ManualHold);
        VERIFY_IS_TRUE(restored->Intent.has_value());
        VERIFY_IS_TRUE(restored->Intent->Stage == TaskIntentStage::Completed);
        VERIFY_ARE_EQUAL(4ull, restored->Intent->Revision);
        VERIFY_ARE_EQUAL(size_t{ 1 }, restored->Runs.size());
        VERIFY_ARE_EQUAL(7ull, restored->Task.Version);
        VERIFY_ARE_EQUAL(7ull, restored->LastEventSequence);
    }

    void TaskCoreRecoveryTests::RecoverySnapshotReturnsBoundedPendingResources()
    {
        TemporaryDatabasePath path;
        const auto firstTask = MakeCreateTaskCommand(1, 1, 1, 1000);
        const auto secondTask = MakeCreateTaskCommand(2, 2, 20, 2000);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(firstTask).Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskTerminalState(
                                MakeTerminalStateCommand(firstTask, 2, 1, 1050, TaskTerminalState::Starting, 1, true))
                           .Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(MakeCreateRunCommand(firstTask, 1, 3, 2, 1100)).Succeeded());
        VERIFY_IS_TRUE(store.StartTaskIntent(
                                MakeStartIntentCommand(firstTask, 1, 4, 3, 1150, TaskIntentKind::Close))
                           .Succeeded());

        VERIFY_IS_TRUE(store.CreateTask(secondTask).Succeeded());
        VERIFY_IS_TRUE(store.StartTaskIntent(
                                MakeStartIntentCommand(secondTask, 2, 21, 1, 2100, TaskIntentKind::Close))
                           .Succeeded());

        auto database = OpenRawDatabase(path.Path());
        const auto firstMessage = MakeId<TaskMessageId>(5001);
        const auto secondMessage = MakeId<TaskMessageId>(5002);
        const auto messageSql =
            "INSERT INTO task_message(id,task_id,state,revision,content,claimed_by_run_id,created_at) VALUES('" +
            firstMessage.Value() + "','" + firstTask.Task.Id.Value() + "',0,1,'first',NULL,1160),('" +
            secondMessage.Value() + "','" + secondTask.Task.Id.Value() + "',1,1,'second',NULL,2110);";
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(database, messageSql.c_str(), nullptr, nullptr, nullptr));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));

        const auto recovery = store.LoadRecoverySnapshot(1);
        VERIFY_IS_TRUE(recovery.Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 1 }, recovery.Snapshot.ActiveRuns.size());
        VERIFY_IS_FALSE(recovery.Snapshot.HasMoreActiveRuns);
        VERIFY_ARE_EQUAL(size_t{ 1 }, recovery.Snapshot.PendingIntents.size());
        VERIFY_IS_TRUE(recovery.Snapshot.HasMorePendingIntents);
        VERIFY_ARE_EQUAL(size_t{ 1 }, recovery.Snapshot.RecoverableTerminals.size());
        VERIFY_IS_FALSE(recovery.Snapshot.HasMoreRecoverableTerminals);
        VERIFY_ARE_EQUAL(size_t{ 1 }, recovery.Snapshot.UnclaimedMessages.size());
        VERIFY_IS_TRUE(recovery.Snapshot.HasMoreUnclaimedMessages);
        VERIFY_ARE_EQUAL(size_t{ 0 }, recovery.Snapshot.Tombstones.size());
    }

    void TaskCoreRecoveryTests::CompletedIntentCanBeReplacedByNewLifecycleWork()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto closeIntent = MakeStartIntentCommand(task, 1, 2, 1, 1100, TaskIntentKind::Close);
        const auto cleanupIntent = MakeStartIntentCommand(task, 2, 4, 3, 1300, TaskIntentKind::WorktreeCleanup);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.StartTaskIntent(closeIntent).Succeeded());
        VERIFY_IS_TRUE(store.AdvanceTaskIntent(
                                MakeAdvanceIntentCommand(closeIntent, 3, 2, 1, 1200, TaskIntentStage::Completed))
                           .Succeeded());

        const auto cleanup = store.StartTaskIntent(cleanupIntent);
        VERIFY_IS_TRUE(cleanup.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(4ull, cleanup.CommittedTaskVersion);
        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Intent.has_value());
        VERIFY_IS_TRUE(snapshot->Intent->Id == cleanupIntent.Intent.Id);
        VERIFY_IS_TRUE(snapshot->Intent->Kind == TaskIntentKind::WorktreeCleanup);
        VERIFY_IS_TRUE(snapshot->Intent->Stage == TaskIntentStage::Requested);
        VERIFY_IS_TRUE(snapshot->Task.ManualHold);
    }

    void TaskCoreRecoveryTests::IntentKindRejectsUnsupportedStageWithoutMutation()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto stopIntent = MakeStartIntentCommand(task, 1, 2, 1, 1100, TaskIntentKind::Stop);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.StartTaskIntent(stopIntent).Succeeded());

        const auto competing = store.StartTaskIntent(
            MakeStartIntentCommand(task, 2, 3, 2, 1150, TaskIntentKind::Close));
        VERIFY_IS_TRUE(competing.Status == TaskStoreCommandStatus::ActiveIntentExists);
        VERIFY_IS_TRUE(competing.Transition == TransitionError::ActiveIntentConflict);

        const auto rejected = store.AdvanceTaskIntent(
            MakeAdvanceIntentCommand(stopIntent, 4, 2, 1, 1200, TaskIntentStage::Checkpointing));
        VERIFY_IS_TRUE(rejected.Status == TaskStoreCommandStatus::InvalidTransition);

        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Intent.has_value());
        VERIFY_IS_TRUE(snapshot->Intent->Stage == TaskIntentStage::Requested);
        VERIFY_ARE_EQUAL(1ull, snapshot->Intent->Revision);
        VERIFY_ARE_EQUAL(2ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(2ull, snapshot->LastEventSequence);
    }

    void TaskCoreRecoveryTests::IncompleteCleanupBlocksRunAndStatusWithoutManualHold()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto cleanup = MakeStartIntentCommand(task, 1, 2, 1, 1100, TaskIntentKind::WorktreeCleanup);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.StartTaskIntent(cleanup).Succeeded());

        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_FALSE(snapshot->Task.ManualHold);

        const auto run = store.CreateTaskRun(MakeCreateRunCommand(task, 1, 3, 2, 1200));
        VERIFY_IS_TRUE(run.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(run.Transition == TransitionError::ActiveIntentConflict);

        const auto status = store.ChangeTaskStatus(
            MakeStatusCommand(task, 4, 2, 1200, TaskStatus::Backlog));
        VERIFY_IS_TRUE(status.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(status.Transition == TransitionError::ActiveIntentConflict);

        const auto retained = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(retained.has_value());
        VERIFY_ARE_EQUAL(2ull, retained->Task.Version);
        VERIFY_ARE_EQUAL(2ull, retained->LastEventSequence);
    }
}
