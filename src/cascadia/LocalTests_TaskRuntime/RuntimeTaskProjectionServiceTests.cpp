// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeTaskProjectionService.h"
#include "TaskCoreStoreTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        TaskUiCommandRequest _create(const uint64_t commandSeed = 1)
        {
            TaskUiCommandRequest request;
            request.CommandId = MakeId<CommandId>(commandSeed).Value();
            request.Operation = TaskUiCommandOperation::Create;
            request.TaskId = MakeId<TaskId>(100 + commandSeed).Value();
            request.Draft = TaskUiCreateDraft{};
            request.Draft->WorkspaceId = MakeId<WorkspaceId>(200).Value();
            request.Draft->Title = "Runtime Task";
            request.Draft->Description = "Persist through the Runtime";
            request.Draft->Acceptance = { "Saved", "Queryable" };
            request.Draft->SourceRoot = "C:/src/repository";
            request.Draft->ExecutionMode = TaskProjectionExecutionMode::Direct;
            request.Draft->Instructions = "Keep changes scoped.";
            return request;
        }

        TaskUiCommandRequest _action(
            const TaskUiCommandRequest& create,
            const uint64_t commandSeed,
            const TaskUiCommandOperation operation,
            const uint64_t version)
        {
            TaskUiCommandRequest request;
            request.CommandId = MakeId<CommandId>(commandSeed).Value();
            request.Operation = operation;
            request.TaskId = create.TaskId;
            request.ExpectedTaskVersion = version;
            return request;
        }
    }

    class RuntimeTaskProjectionServiceTests
    {
        TEST_CLASS(RuntimeTaskProjectionServiceTests);

        TEST_METHOD(CreateQueryStartStopAndCloseUseDurableReceipts);
        TEST_METHOD(QueryFiltersWorkspaceCompletionAndEventTail);
        TEST_METHOD(WorktreeDraftProjectsDeterministicFuturePath);
        TEST_METHOD(RejectsStaleAndInvalidCommandsWithoutMutation);
    };

    void RuntimeTaskProjectionServiceTests::CreateQueryStartStopAndCloseUseDurableReceipts()
    {
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        RuntimeTaskProjectionService service{ store, nullptr, path.Path().parent_path() };
        VERIFY_IS_TRUE(service.Valid());
        const auto create = _create();
        auto result = service.Execute(create, nullptr);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, result.Result.TaskVersion);

        const auto replay = service.Execute(create, nullptr);
        VERIFY_IS_TRUE(replay.Status == RuntimeTaskProjectionStatus::Replayed);
        VERIFY_IS_TRUE(replay.Result.Replayed);
        VERIFY_ARE_EQUAL(create.TaskId, replay.Result.TaskId);

        TaskProjectionQueryRequest query;
        query.TaskId = create.TaskId;
        query.IncludeCompleted = true;
        query.EventLimit = 32;
        auto queried = service.Query(query);
        VERIFY_IS_TRUE(queried.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, queried.Projection.Tasks.size());
        VERIFY_ARE_EQUAL(std::string{ "Runtime Task" }, queried.Projection.Tasks.front().Title);

        const auto start = _action(create, 2, TaskUiCommandOperation::Start, 1);
        result = service.Execute(start, nullptr);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(result.Result.RunId.has_value());
        VERIFY_IS_TRUE(result.Result.Pending);
        const auto originalRun = result.Result.RunId;
        const auto startReplay = service.Execute(start, nullptr);
        VERIFY_IS_TRUE(startReplay.Status == RuntimeTaskProjectionStatus::Replayed);
        VERIFY_IS_TRUE(startReplay.Result.RunId == originalRun);

        const auto stop = _action(create, 3, TaskUiCommandOperation::Stop, 2);
        result = service.Execute(stop, nullptr);
        VERIFY_IS_TRUE(result.Succeeded());
        auto aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_IS_TRUE(aggregate->Runs.front().State == TaskRunState::Cancelled);

        const auto close = _action(create, 4, TaskUiCommandOperation::Close, aggregate->Task.Version);
        result = service.Execute(close, nullptr);
        VERIFY_IS_TRUE(result.Succeeded());
        aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_IS_TRUE(aggregate->Terminal.State == TaskTerminalState::Closed);
    }

    void RuntimeTaskProjectionServiceTests::QueryFiltersWorkspaceCompletionAndEventTail()
    {
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        RuntimeTaskProjectionService service{ store, nullptr, path.Path().parent_path() };
        const auto first = _create(10);
        auto second = _create(20);
        second.Draft->WorkspaceId = MakeId<WorkspaceId>(201).Value();
        VERIFY_IS_TRUE(service.Execute(first, nullptr).Succeeded());
        VERIFY_IS_TRUE(service.Execute(second, nullptr).Succeeded());

        TaskProjectionQueryRequest query;
        query.WorkspaceId = first.Draft->WorkspaceId;
        query.EventLimit = 1;
        auto result = service.Query(query);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, result.Projection.Tasks.size());
        VERIFY_ARE_EQUAL(first.TaskId, result.Projection.Tasks.front().Id);
        VERIFY_ARE_EQUAL(size_t{ 1 }, result.Projection.Tasks.front().Events.size());

        query.WorkspaceId.reset();
        result = service.Query(query);
        VERIFY_ARE_EQUAL(size_t{ 2 }, result.Projection.Tasks.size());
    }

    void RuntimeTaskProjectionServiceTests::WorktreeDraftProjectsDeterministicFuturePath()
    {
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        RuntimeTaskProjectionService service{ store, nullptr, LR"(C:\runtime-data)" };
        auto create = _create(30);
        create.Draft->ExecutionMode = TaskProjectionExecutionMode::Worktree;
        VERIFY_IS_TRUE(service.Execute(create, nullptr).Succeeded());
        TaskProjectionQueryRequest query;
        query.TaskId = create.TaskId;
        query.IncludeCompleted = true;
        const auto projected = service.Query(query);
        VERIFY_IS_TRUE(projected.Succeeded());
        const auto& execution = projected.Projection.Tasks.front().Execution;
        VERIFY_IS_TRUE(execution.Mode == TaskProjectionExecutionMode::Worktree);
        VERIFY_IS_TRUE(execution.WorktreePath.find(create.TaskId) != std::string::npos);
        VERIFY_IS_TRUE(execution.Branch.find(create.TaskId) != std::string::npos);
    }

    void RuntimeTaskProjectionServiceTests::RejectsStaleAndInvalidCommandsWithoutMutation()
    {
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        RuntimeTaskProjectionService service{ store, nullptr, path.Path().parent_path() };
        const auto create = _create(40);
        VERIFY_IS_TRUE(service.Execute(create, nullptr).Succeeded());
        auto stale = _action(create, 41, TaskUiCommandOperation::Start, 99);
        VERIFY_IS_TRUE(service.Execute(stale, nullptr).Status == RuntimeTaskProjectionStatus::Conflict);
        auto aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_ARE_EQUAL(size_t{ 0 }, aggregate->Runs.size());

        stale.TaskId = "not-a-uuid";
        VERIFY_IS_TRUE(service.Execute(stale, nullptr).Status == RuntimeTaskProjectionStatus::InvalidRequest);
        TaskProjectionQueryRequest invalid;
        invalid.TaskId = create.TaskId;
        invalid.WorkspaceId = create.Draft->WorkspaceId;
        VERIFY_IS_TRUE(service.Query(invalid).Status == RuntimeTaskProjectionStatus::InvalidRequest);
    }
}
