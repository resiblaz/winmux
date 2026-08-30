// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskCoreStoreTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    namespace
    {
        StartTaskIntentCommand _advanceDeleteToAggregate(
            SqliteTaskStore& store,
            const CreateTaskCommand& task,
            const uint64_t intentSeed,
            const uint64_t startCommandSeed,
            const uint64_t advanceCommandSeed,
            const uint64_t expectedTaskVersion,
            const int64_t timestamp)
        {
            const auto intent = MakeStartIntentCommand(
                task,
                intentSeed,
                startCommandSeed,
                expectedTaskVersion,
                timestamp,
                TaskIntentKind::Delete);
            VERIFY_IS_TRUE(store.StartTaskIntent(intent).Succeeded());
            VERIFY_IS_TRUE(store.AdvanceTaskIntent(
                                    MakeAdvanceIntentCommand(
                                        intent,
                                        advanceCommandSeed,
                                        expectedTaskVersion + 1,
                                        1,
                                        timestamp + 10,
                                        TaskIntentStage::DeletingAggregate))
                               .Succeeded());
            return intent;
        }
    }

    class TaskCoreDeletionTests
    {
        TEST_CLASS(TaskCoreDeletionTests);

        TEST_METHOD(DeleteIsAtomicReplayableAndRetainedForThirtyDays);
        TEST_METHOD(DeleteRequiresReadyIntentAndNoActiveRun);
        TEST_METHOD(TombstoneCommandConflictDoesNotMutateAggregate);
        TEST_METHOD(TombstoneCleanupIsBoundedAndReportsRemainingWork);
    };

    void TaskCoreDeletionTests::DeleteIsAtomicReplayableAndRetainedForThirtyDays()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        const auto intent = _advanceDeleteToAggregate(store, task, 1, 2, 3, 1, 1100);
        const auto command = MakeDeleteTaskCommand(intent, 4, 3, 2, 1300);

        const auto deleted = store.DeleteTask(command);
        VERIFY_IS_TRUE(deleted.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(4ull, deleted.CommittedTaskVersion);
        VERIFY_ARE_EQUAL(4ull, deleted.LastEventSequence);
        VERIFY_IS_FALSE(store.LoadTask(task.Task.Id).has_value());
        VERIFY_ARE_EQUAL(size_t{ 0 }, store.ListTasks(std::nullopt).size());

        const auto tombstone = store.LoadDeleteTombstone(task.Task.Id);
        VERIFY_IS_TRUE(tombstone.has_value());
        VERIFY_ARE_EQUAL(1300ll, tombstone->DeletedAtUnixMilliseconds);
        VERIFY_ARE_EQUAL(1300ll + TaskTombstoneRetentionMilliseconds, tombstone->ExpiresAtUnixMilliseconds);
        VERIFY_IS_TRUE(tombstone->Command == std::optional<CommandId>{ command.Metadata.Command });
        VERIFY_IS_TRUE(tombstone->ExpectedTaskVersion == std::optional<uint64_t>{ 3 });
        VERIFY_IS_TRUE(tombstone->CommittedTaskVersion == std::optional<uint64_t>{ 4 });
        VERIFY_IS_TRUE(tombstone->LastEventSequence == std::optional<uint64_t>{ 4 });
        VERIFY_ARE_EQUAL(command.Metadata.RequestDigest, tombstone->ReplayDigest);

        const auto replay = store.DeleteTask(command);
        VERIFY_IS_TRUE(replay.Status == TaskStoreCommandStatus::Replayed);
        VERIFY_ARE_EQUAL(4ull, replay.CommittedTaskVersion);
        VERIFY_ARE_EQUAL(4ull, replay.LastEventSequence);

        auto conflict = command;
        conflict.Metadata.RequestDigest.assign(64, 'f');
        VERIFY_IS_TRUE(store.DeleteTask(conflict).Status == TaskStoreCommandStatus::IdempotencyConflict);

        const auto anotherDelete = MakeDeleteTaskCommand(intent, 5, 3, 2, 1400);
        VERIFY_IS_TRUE(store.DeleteTask(anotherDelete).Status == TaskStoreCommandStatus::Tombstoned);
        VERIFY_IS_TRUE(store.CreateTask(task).Status == TaskStoreCommandStatus::Tombstoned);
        VERIFY_IS_TRUE(store.ChangeTaskManualHold(
                                MakeManualHoldCommand(task, 6, 4, 1450, false))
                           .Status == TaskStoreCommandStatus::Tombstoned);

        auto reusedCommand = MakeCreateTaskCommand(99, 99, 99, 1500);
        reusedCommand.Metadata.Command = command.Metadata.Command;
        VERIFY_IS_TRUE(store.CreateTask(reusedCommand).Status == TaskStoreCommandStatus::IdempotencyConflict);
        VERIFY_IS_FALSE(store.LoadTask(reusedCommand.Task.Id).has_value());

        const auto recovery = store.LoadRecoverySnapshot(10);
        VERIFY_IS_TRUE(recovery.Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 1 }, recovery.Snapshot.Tombstones.size());

        const auto beforeExpiry = store.PurgeExpiredTombstones(
            tombstone->ExpiresAtUnixMilliseconds - 1,
            10);
        VERIFY_IS_TRUE(beforeExpiry.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 0 }, beforeExpiry.RemovedCount);
        VERIFY_IS_TRUE(store.LoadDeleteTombstone(task.Task.Id).has_value());

        const auto expired = store.PurgeExpiredTombstones(tombstone->ExpiresAtUnixMilliseconds, 10);
        VERIFY_IS_TRUE(expired.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, expired.RemovedCount);
        VERIFY_IS_FALSE(expired.HasMore);
        VERIFY_IS_FALSE(store.LoadDeleteTombstone(task.Task.Id).has_value());
    }

    void TaskCoreDeletionTests::DeleteRequiresReadyIntentAndNoActiveRun()
    {
        TemporaryDatabasePath path;
        const auto firstTask = MakeCreateTaskCommand(1, 1, 1, 1000);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(firstTask).Succeeded());
        const auto requested = MakeStartIntentCommand(firstTask, 1, 2, 1, 1100, TaskIntentKind::Delete);
        VERIFY_IS_TRUE(store.StartTaskIntent(requested).Succeeded());
        const auto tooEarly = store.DeleteTask(MakeDeleteTaskCommand(requested, 3, 2, 1, 1200));
        VERIFY_IS_TRUE(tooEarly.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(store.LoadTask(firstTask.Task.Id).has_value());

        const auto secondTask = MakeCreateTaskCommand(2, 2, 20, 2000);
        VERIFY_IS_TRUE(store.CreateTask(secondTask).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(MakeCreateRunCommand(secondTask, 2, 21, 1, 2100)).Succeeded());
        const auto activeIntent = _advanceDeleteToAggregate(store, secondTask, 2, 22, 23, 2, 2200);
        const auto activeDelete = store.DeleteTask(MakeDeleteTaskCommand(activeIntent, 24, 4, 2, 2300));
        VERIFY_IS_TRUE(activeDelete.Status == TaskStoreCommandStatus::ActiveRunExists);
        const auto snapshot = store.LoadTask(secondTask.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(4ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(4ull, snapshot->LastEventSequence);
    }

    void TaskCoreDeletionTests::TombstoneCommandConflictDoesNotMutateAggregate()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        const auto firstTask = MakeCreateTaskCommand(1, 1, 1, 1000);
        VERIFY_IS_TRUE(store.CreateTask(firstTask).Succeeded());
        const auto firstIntent = _advanceDeleteToAggregate(store, firstTask, 1, 2, 3, 1, 1100);
        const auto firstDelete = MakeDeleteTaskCommand(firstIntent, 4, 3, 2, 1300);
        VERIFY_IS_TRUE(store.DeleteTask(firstDelete).Succeeded());

        const auto secondTask = MakeCreateTaskCommand(2, 2, 20, 2000);
        VERIFY_IS_TRUE(store.CreateTask(secondTask).Succeeded());
        const auto secondIntent = _advanceDeleteToAggregate(store, secondTask, 2, 21, 22, 1, 2100);
        auto conflictingDelete = MakeDeleteTaskCommand(secondIntent, 23, 3, 2, 2300);
        conflictingDelete.Metadata.Command = firstDelete.Metadata.Command;

        const auto failed = store.DeleteTask(conflictingDelete);
        VERIFY_IS_TRUE(failed.Status == TaskStoreCommandStatus::IdempotencyConflict);
        VERIFY_IS_FALSE(store.LoadDeleteTombstone(secondTask.Task.Id).has_value());
        const auto retained = store.LoadTask(secondTask.Task.Id);
        VERIFY_IS_TRUE(retained.has_value());
        VERIFY_ARE_EQUAL(3ull, retained->Task.Version);
        VERIFY_ARE_EQUAL(3ull, retained->LastEventSequence);
    }

    void TaskCoreDeletionTests::TombstoneCleanupIsBoundedAndReportsRemainingWork()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        int64_t latestExpiry{};
        for (uint64_t index = 1; index <= 3; ++index)
        {
            const auto commandSeed = index * 10;
            const auto timestamp = static_cast<int64_t>(1000 + index * 100);
            const auto task = MakeCreateTaskCommand(index, index, commandSeed + 1, timestamp);
            VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
            const auto intent = _advanceDeleteToAggregate(
                store,
                task,
                index,
                commandSeed + 2,
                commandSeed + 3,
                1,
                timestamp + 10);
            VERIFY_IS_TRUE(store.DeleteTask(
                                    MakeDeleteTaskCommand(intent, commandSeed + 4, 3, 2, timestamp + 30))
                               .Succeeded());
            latestExpiry = timestamp + 30 + TaskTombstoneRetentionMilliseconds;
        }

        const auto firstBatch = store.PurgeExpiredTombstones(latestExpiry, 2);
        VERIFY_IS_TRUE(firstBatch.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 2 }, firstBatch.RemovedCount);
        VERIFY_IS_TRUE(firstBatch.HasMore);

        const auto secondBatch = store.PurgeExpiredTombstones(latestExpiry, 2);
        VERIFY_IS_TRUE(secondBatch.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, secondBatch.RemovedCount);
        VERIFY_IS_FALSE(secondBatch.HasMore);
        VERIFY_ARE_EQUAL(size_t{ 0 }, store.LoadRecoverySnapshot(10).Snapshot.Tombstones.size());
    }
}
