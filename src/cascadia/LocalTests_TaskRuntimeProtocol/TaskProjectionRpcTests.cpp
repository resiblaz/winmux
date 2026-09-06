// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskProjectionRpc.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        TaskProjectionRecord _task()
        {
            TaskProjectionRecord task;
            task.Id = "10000000-0000-4000-8000-000000000001";
            task.WorkspaceId = "20000000-0000-4000-8000-000000000002";
            task.Title = "Runtime-backed Task";
            task.Description = "Projection details";
            task.Acceptance = { { "Works", true } };
            task.Status = TaskProjectionStatus::InProgress;
            task.Priority = TaskProjectionPriority::High;
            task.Version = 8;
            task.CreatedAtUnixMilliseconds = 1000;
            task.UpdatedAtUnixMilliseconds = 1800;
            task.Execution.Id = "30000000-0000-4000-8000-000000000003";
            task.Execution.Revision = 1;
            task.Execution.Mode = TaskProjectionExecutionMode::Worktree;
            task.Execution.SourceRoot = "C:/src/repo";
            task.Execution.RuntimeRoot = "C:/runtime/task";
            task.Execution.WorktreePath = "C:/runtime/task";
            task.Execution.Branch = "refs/heads/wt-task/id";
            task.Execution.Provider = "codex";
            task.Terminal.State = TaskProjectionTerminalState::Busy;
            task.Terminal.Generation = 1;
            task.Terminal.CurrentAgentSessionId = "40000000-0000-4000-8000-000000000004";
            task.AgentSessions.push_back({ *task.Terminal.CurrentAgentSessionId, "codex", "provider-thread", 0, true });
            TaskProjectionRunRecord run;
            run.Id = "50000000-0000-4000-8000-000000000005";
            run.ExecutionContextId = task.Execution.Id;
            run.AgentSessionId = task.Terminal.CurrentAgentSessionId;
            run.State = TaskProjectionRunState::Running;
            run.Sequence = 1;
            run.CreatedAtUnixMilliseconds = 1200;
            run.StartedAtUnixMilliseconds = 1300;
            task.Runs.push_back(run);
            task.Messages.push_back({ "60000000-0000-4000-8000-000000000006", 0, 1, 1, "later", std::nullopt, 1400, 1400 });
            task.Events.push_back({ "70000000-0000-4000-8000-000000000007", run.Id, 8, 4, "Provider_tool_progress", "{}", 1500 });
            task.LastEventSequence = 8;
            return task;
        }
    }

    class TaskProjectionRpcTests
    {
        TEST_CLASS(TaskProjectionRpcTests);

        TEST_METHOD(QueryAndProjectionRoundTripStrictly);
        TEST_METHOD(RemovedMessagesAllowEmptyContentWithoutHidingTasks);
        TEST_METHOD(UiCommandsRoundTripByOperation);
        TEST_METHOD(RejectsMalformedOversizedAndNoncanonicalPayloads);
        TEST_METHOD(AgentConfigurationAndHistoricalSnapshotsAreVersionedAndOptIn);
    };

    void TaskProjectionRpcTests::AgentConfigurationAndHistoricalSnapshotsAreVersionedAndOptIn()
    {
        TaskUiCommandRequest command;
        command.CommandId = "80000000-0000-4000-8000-000000000008";
        command.TaskId = "10000000-0000-4000-8000-000000000001";
        command.Operation = TaskUiCommandOperation::UpdateAgent; command.ExpectedTaskVersion = 5;
        command.AgentConfiguration = TaskAgentConfiguration{};
        command.AgentConfiguration->Agent = AgentProfileReference{ "90000000-0000-4000-8000-000000000009", 2 };
        const auto decoded = DecodeTaskUiCommandRequest(EncodeTaskUiCommandRequest(command).Payload);
        VERIFY_IS_TRUE(decoded.Succeeded()); VERIFY_ARE_EQUAL(uint64_t{ 2 }, decoded.Value.AgentConfiguration->Agent->Revision);
        command.AgentConfiguration->Model = "forbidden-override";
        VERIFY_IS_FALSE(EncodeTaskUiCommandRequest(command).Succeeded);
        auto task = _task();
        task.Execution.Agent = AgentProfileReference{ "90000000-0000-4000-8000-000000000009", 2 };
        task.Execution.AgentName = "Reviewer"; task.Execution.Model = "new-model";
        task.Runs.front().Agent = AgentProfileReference{ task.Execution.Agent->Id, 1 };
        task.Runs.front().AgentName = "Old reviewer"; task.Runs.front().Model = "old-model";
        task.Runs.front().Provider = "codex"; task.Runs.front().ExecutionRevision = 1;
        TaskProjectionQueryResult projection{ 1, { task } };
        const auto extended = DecodeTaskProjectionQueryResult(EncodeTaskProjectionQueryResult(projection, true).Payload);
        VERIFY_IS_TRUE(extended.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "old-model" }, extended.Value.Tasks.front().Runs.front().Model);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, extended.Value.Tasks.front().Runs.front().Agent->Revision);
        const auto legacy = EncodeTaskProjectionQueryResult(projection);
        VERIFY_IS_TRUE(legacy.Succeeded); VERIFY_IS_TRUE(legacy.Payload.find("executionAgent") == std::string::npos);
        VERIFY_IS_FALSE(DecodeTaskProjectionQueryResult(legacy.Payload).Value.Tasks.front().Execution.Agent.has_value());
    }

    void TaskProjectionRpcTests::RemovedMessagesAllowEmptyContentWithoutHidingTasks()
    {
        TaskProjectionQueryResult projection;
        projection.Tasks.push_back(_task());
        auto& message = projection.Tasks.front().Messages.front();
        message.State = 3;
        message.Content.clear();
        const auto encoded = EncodeTaskProjectionQueryResult(projection);
        VERIFY_IS_TRUE(encoded.Succeeded);
        const auto decoded = DecodeTaskProjectionQueryResult(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, decoded.Value.Tasks.size());
        VERIFY_IS_TRUE(decoded.Value.Tasks.front().Messages.front().Content.empty());
        message.State = 1;
        VERIFY_IS_FALSE(EncodeTaskProjectionQueryResult(projection).Succeeded);
    }

    void TaskProjectionRpcTests::QueryAndProjectionRoundTripStrictly()
    {
        TaskProjectionQueryRequest query;
        query.WorkspaceId = "20000000-0000-4000-8000-000000000002";
        query.IncludeCompleted = true;
        query.EventLimit = 100;
        auto encoded = EncodeTaskProjectionQueryRequest(query);
        VERIFY_IS_TRUE(encoded.Succeeded);
        auto decoded = DecodeTaskProjectionQueryRequest(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_IS_TRUE(decoded.Value.WorkspaceId == query.WorkspaceId);
        VERIFY_ARE_EQUAL(size_t{ 100 }, decoded.Value.EventLimit);

        TaskProjectionQueryResult projection;
        projection.Generation = 9;
        projection.Tasks.push_back(_task());
        encoded = EncodeTaskProjectionQueryResult(projection);
        VERIFY_IS_TRUE(encoded.Succeeded);
        const auto roundtrip = DecodeTaskProjectionQueryResult(encoded.Payload);
        VERIFY_IS_TRUE(roundtrip.Succeeded());
        VERIFY_ARE_EQUAL(uint64_t{ 9 }, roundtrip.Value.Generation);
        VERIFY_ARE_EQUAL(size_t{ 1 }, roundtrip.Value.Tasks.size());
        VERIFY_ARE_EQUAL(std::string{ "Runtime-backed Task" }, roundtrip.Value.Tasks.front().Title);
        VERIFY_ARE_EQUAL(std::string{ "Provider_tool_progress" }, roundtrip.Value.Tasks.front().Events.front().Type);
    }

    void TaskProjectionRpcTests::UiCommandsRoundTripByOperation()
    {
        TaskUiCommandRequest create;
        create.CommandId = "80000000-0000-4000-8000-000000000008";
        create.TaskId = "10000000-0000-4000-8000-000000000001";
        create.Operation = TaskUiCommandOperation::Create;
        create.Draft = TaskUiCreateDraft{};
        create.Draft->WorkspaceId = "20000000-0000-4000-8000-000000000002";
        create.Draft->Title = "Create me";
        create.Draft->Description = "Goal";
        create.Draft->Acceptance = { "First", "Second" };
        create.Draft->SourceRoot = "C:/src/repo";
        auto encoded = EncodeTaskUiCommandRequest(create);
        VERIFY_IS_TRUE(encoded.Succeeded);
        auto decoded = DecodeTaskUiCommandRequest(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_IS_TRUE(decoded.Value.Operation == TaskUiCommandOperation::Create);
        VERIFY_ARE_EQUAL(size_t{ 2 }, decoded.Value.Draft->Acceptance.size());

        create.Operation = TaskUiCommandOperation::Start;
        create.ExpectedTaskVersion = 1;
        create.Draft.reset();
        encoded = EncodeTaskUiCommandRequest(create);
        VERIFY_IS_TRUE(encoded.Succeeded);
        decoded = DecodeTaskUiCommandRequest(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Value.Operation == TaskUiCommandOperation::Start);

        create.Operation = TaskUiCommandOperation::QueueMessage;
        create.Text = "follow up";
        encoded = EncodeTaskUiCommandRequest(create);
        VERIFY_IS_TRUE(encoded.Succeeded);
        decoded = DecodeTaskUiCommandRequest(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Value.Operation == TaskUiCommandOperation::QueueMessage);
        VERIFY_IS_TRUE(decoded.Value.Text == create.Text);
        create.Text.reset();

        TaskUiCommandResult result;
        result.CommandId = create.CommandId;
        result.TaskId = create.TaskId;
        result.RunId = "50000000-0000-4000-8000-000000000005";
        result.TaskVersion = 2;
        result.EventSequence = 2;
        result.Pending = true;
        result.Surface = TaskUiSurfaceDescriptor{
            "60000000-0000-4000-8000-000000000006",
            R"(\\.\pipe\WT.TaskRuntime.test.terminal)",
            3,
            false,
        };
        encoded = EncodeTaskUiCommandResult(result);
        VERIFY_IS_TRUE(encoded.Succeeded);
        const auto resultRoundtrip = DecodeTaskUiCommandResult(encoded.Payload);
        VERIFY_IS_TRUE(resultRoundtrip.Succeeded());
        VERIFY_IS_TRUE(resultRoundtrip.Value.Pending);
        VERIFY_IS_TRUE(resultRoundtrip.Value.RunId == result.RunId);
        VERIFY_IS_TRUE(resultRoundtrip.Value.Surface.has_value());
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, resultRoundtrip.Value.Surface->Generation);
    }

    void TaskProjectionRpcTests::RejectsMalformedOversizedAndNoncanonicalPayloads()
    {
        VERIFY_IS_FALSE(DecodeTaskProjectionQueryRequest("{}").Succeeded());
        VERIFY_IS_FALSE(DecodeTaskProjectionQueryRequest(R"({"taskId":null,"workspaceId":null,"includeCompleted":true,"eventLimit":1,"extra":1})").Succeeded());
        VERIFY_IS_FALSE(DecodeTaskUiCommandRequest("[]").Succeeded());

        auto task = _task();
        task.Id = "not-a-uuid";
        TaskProjectionQueryResult result;
        result.Tasks.push_back(std::move(task));
        VERIFY_IS_FALSE(EncodeTaskProjectionQueryResult(result).Succeeded);

        TaskProjectionQueryRequest query;
        query.EventLimit = MaximumTaskProjectionEvents + 1;
        VERIFY_IS_FALSE(EncodeTaskProjectionQueryRequest(query).Succeeded);
    }
}
