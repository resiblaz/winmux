// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskCoreStoreTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    class TaskCoreStoreTaskTests
    {
        TEST_CLASS(TaskCoreStoreTaskTests);

        TEST_METHOD(PersistsCompleteTaskAggregateAndReopens);
        TEST_METHOD(CommandReceiptReplayAndConflictAreDurable);
        TEST_METHOD(VersionConflictDoesNotMutateAndEventsPageInOrder);
        TEST_METHOD(FailedAggregateInsertRollsBackEveryRow);
        TEST_METHOD(ListTasksFiltersWorkspaceAndIncludesCriteria);
    };

    void TaskCoreStoreTaskTests::PersistsCompleteTaskAggregateAndReopens()
    {
        TemporaryDatabasePath path;
        const auto create = MakeCreateTaskCommand();
        {
            SqliteTaskStore store{ path.Path() };
            const auto result = store.CreateTask(create);
            VERIFY_IS_TRUE(result.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(1ull, result.CommittedTaskVersion);
            VERIFY_ARE_EQUAL(1ull, result.LastEventSequence);
            VERIFY_ARE_EQUAL(std::string{ "{\"accepted\":true}" }, result.ResultJson);

            const auto snapshot = store.LoadTask(create.Task.Id);
            VERIFY_IS_TRUE(snapshot.has_value());
            VERIFY_IS_TRUE(snapshot->Task.Id == create.Task.Id);
            VERIFY_IS_TRUE(snapshot->Task.Workspace == create.Task.Workspace);
            VERIFY_ARE_EQUAL(create.Task.Title, snapshot->Task.Title);
            VERIFY_ARE_EQUAL(size_t{ 2 }, snapshot->Task.AcceptanceCriteria.size());
            VERIFY_ARE_EQUAL(std::string{ "State is durable" }, snapshot->Task.AcceptanceCriteria[0].Text);
            VERIFY_IS_TRUE(snapshot->Task.AcceptanceCriteria[1].Satisfied);
            VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->ExecutionContexts.size());
            VERIFY_IS_TRUE(snapshot->ExecutionContexts[0].Id == create.ExecutionContext.Id);
            VERIFY_IS_TRUE(snapshot->Terminal.Id == create.Terminal.Id);
            VERIFY_IS_TRUE(snapshot->Terminal.State == TaskTerminalState::NotCreated);
            VERIFY_ARE_EQUAL(size_t{ 0 }, snapshot->Runs.size());
            VERIFY_ARE_EQUAL(1ull, snapshot->LastEventSequence);

            const auto events = store.ListEvents(create.Task.Id, 0, 10);
            VERIFY_ARE_EQUAL(size_t{ 1 }, events.size());
            VERIFY_ARE_EQUAL(1ull, events[0].AggregateSequence);
            VERIFY_ARE_EQUAL(std::string{ "TaskCreated" }, events[0].Type);
            VERIFY_IS_FALSE(events[0].RunId.has_value());
        }

        SqliteTaskStore reopened{ path.Path() };
        const auto restored = reopened.LoadTask(create.Task.Id);
        VERIFY_IS_TRUE(restored.has_value());
        VERIFY_ARE_EQUAL(create.Task.Description, restored->Task.Description);
        VERIFY_ARE_EQUAL(size_t{ 2 }, restored->Task.AcceptanceCriteria.size());
        VERIFY_ARE_EQUAL(1ull, restored->Task.Version);
        VERIFY_ARE_EQUAL(1ull, restored->LastEventSequence);
    }

    void TaskCoreStoreTaskTests::CommandReceiptReplayAndConflictAreDurable()
    {
        TemporaryDatabasePath path;
        const auto create = MakeCreateTaskCommand();
        {
            SqliteTaskStore store{ path.Path() };
            VERIFY_IS_TRUE(store.CreateTask(create).Status == TaskStoreCommandStatus::Succeeded);

            const auto replay = store.CreateTask(create);
            VERIFY_IS_TRUE(replay.Status == TaskStoreCommandStatus::Replayed);
            VERIFY_ARE_EQUAL(1ull, replay.CommittedTaskVersion);
            VERIFY_ARE_EQUAL(1ull, replay.LastEventSequence);
            VERIFY_ARE_EQUAL(std::string{ "{\"accepted\":true}" }, replay.ResultJson);

            auto replayWithDiscardedBody = create;
            replayWithDiscardedBody.Task.Title.clear();
            VERIFY_IS_TRUE(store.CreateTask(replayWithDiscardedBody).Status == TaskStoreCommandStatus::Replayed);

            auto conflict = create;
            conflict.Metadata.RequestDigest.assign(64, 'f');
            const auto rejected = store.CreateTask(conflict);
            VERIFY_IS_TRUE(rejected.Status == TaskStoreCommandStatus::IdempotencyConflict);
            VERIFY_ARE_EQUAL(size_t{ 1 }, store.ListEvents(create.Task.Id, 0, 10).size());
            VERIFY_ARE_EQUAL(size_t{ 1 }, store.ListTasks(std::nullopt).size());
        }

        SqliteTaskStore reopened{ path.Path() };
        const auto replay = reopened.CreateTask(create);
        VERIFY_IS_TRUE(replay.Status == TaskStoreCommandStatus::Replayed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, reopened.ListEvents(create.Task.Id, 0, 10).size());
    }

    void TaskCoreStoreTaskTests::VersionConflictDoesNotMutateAndEventsPageInOrder()
    {
        TemporaryDatabasePath path;
        const auto create = MakeCreateTaskCommand();
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(create).Succeeded());

        const auto start = MakeStatusCommand(
            create,
            2,
            1,
            1100,
            TaskStatus::InProgress,
            TaskStatusAuthority::Agent);
        const auto started = store.ChangeTaskStatus(start);
        VERIFY_IS_TRUE(started.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(2ull, started.CommittedTaskVersion);
        VERIFY_ARE_EQUAL(2ull, started.LastEventSequence);

        const auto stale = MakeStatusCommand(create, 3, 1, 1200, TaskStatus::Cancelled);
        const auto rejected = store.ChangeTaskStatus(stale);
        VERIFY_IS_TRUE(rejected.Status == TaskStoreCommandStatus::VersionConflict);

        const auto snapshot = store.LoadTask(create.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Task.Status == TaskStatus::InProgress);
        VERIFY_ARE_EQUAL(2ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(2ull, snapshot->LastEventSequence);

        const auto page = store.ListEvents(create.Task.Id, 1, 1);
        VERIFY_ARE_EQUAL(size_t{ 1 }, page.size());
        VERIFY_ARE_EQUAL(2ull, page[0].AggregateSequence);
        VERIFY_ARE_EQUAL(std::string{ "TaskStatusChanged" }, page[0].Type);
        VERIFY_ARE_EQUAL(size_t{ 0 }, store.ListEvents(create.Task.Id, 2, 10).size());
    }

    void TaskCoreStoreTaskTests::FailedAggregateInsertRollsBackEveryRow()
    {
        TemporaryDatabasePath path;
        const auto first = MakeCreateTaskCommand(1, 1, 1, 1000);
        auto second = MakeCreateTaskCommand(2, 2, 2, 1100);
        second.Terminal.Id = first.Terminal.Id;

        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(first).Succeeded());
        const auto failed = store.CreateTask(second);
        VERIFY_IS_TRUE(failed.Status == TaskStoreCommandStatus::StorageError);
        VERIFY_IS_FALSE(store.LoadTask(second.Task.Id).has_value());
        VERIFY_ARE_EQUAL(size_t{ 0 }, store.ListEvents(second.Task.Id, 0, 10).size());
        VERIFY_ARE_EQUAL(size_t{ 1 }, store.ListTasks(std::nullopt).size());

        second.Terminal.Id = MakeId<TaskTerminalId>(2002);
        const auto retried = store.CreateTask(second);
        VERIFY_IS_TRUE(retried.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 2 }, store.ListTasks(std::nullopt).size());
    }

    void TaskCoreStoreTaskTests::ListTasksFiltersWorkspaceAndIncludesCriteria()
    {
        TemporaryDatabasePath path;
        const auto first = MakeCreateTaskCommand(1, 1, 1, 1000);
        auto second = MakeCreateTaskCommand(2, 2, 2, 1100);
        second.Task.Title = "Second workspace Task";

        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(first).Succeeded());
        VERIFY_IS_TRUE(store.CreateTask(second).Succeeded());

        const auto all = store.ListTasks(std::nullopt);
        VERIFY_ARE_EQUAL(size_t{ 2 }, all.size());
        VERIFY_IS_TRUE(all[0].Id == second.Task.Id);
        VERIFY_ARE_EQUAL(size_t{ 2 }, all[0].AcceptanceCriteria.size());

        const auto firstWorkspace = store.ListTasks(std::optional<WorkspaceId>{ first.Task.Workspace });
        VERIFY_ARE_EQUAL(size_t{ 1 }, firstWorkspace.size());
        VERIFY_IS_TRUE(firstWorkspace[0].Id == first.Task.Id);
        VERIFY_ARE_EQUAL(size_t{ 2 }, firstWorkspace[0].AcceptanceCriteria.size());

        const auto missing = store.ListTasks(std::optional<WorkspaceId>{ MakeId<WorkspaceId>(999) });
        VERIFY_ARE_EQUAL(size_t{ 0 }, missing.size());
    }
}
