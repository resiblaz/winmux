// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "RuntimeTaskBoardService.h"
#include "RuntimeCrypto.h"
#include "RuntimeTaskCommandMetadata.h"
#include "RuntimeTaskOrchestrator.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;
    static_assert(MaximumTaskSummaryItems == MaximumTaskSummaryPageSize);
    RuntimeWorkspaceResult<TaskSessionIndexQueryResult> RuntimeTaskBoardService::QuerySessions(const TaskSessionIndexQueryRequest& request)
    {
        RuntimeWorkspaceResult<TaskSessionIndexQueryResult> result;
        if (!Valid()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = "Task session index is unavailable"; return result; }
        const auto checked = EncodeTaskSessionIndexQueryRequest(request);
        if (!checked.Succeeded) { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = checked.Error; return result; }
        const auto stored = _store->QueryTaskSessionIndex(request.AfterSequence, request.Limit);
        result.Status = stored.Status; result.Error = stored.Error;
        result.Value.AfterSequence = stored.AfterSequence; result.Value.HasMore = stored.HasMore;
        for (const auto& session : stored.Sessions) result.Value.Sessions.push_back({ session.Sequence, session.ProviderSessionId });
        return result;
    }

    RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskSummaryQueryResult> RuntimeTaskBoardService::Query(const TaskSummaryQueryRequest& request)
    {
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskSummaryQueryResult> result;
        if (!Valid()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = "Task board is unavailable"; return result; }
        const auto checked = EncodeTaskSummaryQueryRequest(request);
        if (!checked.Succeeded) { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = checked.Error; return result; }
        TaskSummaryQuery query;
        if (request.ProjectId) query.Workspace = WorkspaceId{ *request.ProjectId };
        if (request.Status) query.Status = static_cast<TaskStatus>(*request.Status);
        if (request.Bucket) query.Bucket = static_cast<TaskStatus>(*request.Bucket);
        query.OpenOnly = request.OpenOnly; query.Search = request.Search; query.Sort = static_cast<TaskSummarySort>(request.Sort);
        query.AfterId = request.AfterId; query.AfterValue = request.AfterValue; query.ExpectedColumnRevision = request.ExpectedColumnRevision; query.Limit = request.Limit;
        const auto stored = _store->QueryTaskSummaries(query);
        result.Status = stored.Status; result.Error = stored.Error;
        if (!stored.Succeeded()) return result;
        auto& page = result.Value;
        page.Counts = stored.Counts; page.Total = stored.Total; page.Generation = stored.Generation; page.ColumnRevision = stored.ColumnRevision;
        page.NextAfterId = stored.NextAfterId; page.NextAfterValue = stored.NextAfterValue;
        for (const auto& task : stored.Tasks)
        {
            TaskSummaryProjection item;
            item.Id = task.Id.Value(); item.ProjectId = task.Workspace.Value(); item.ProjectName = task.ProjectName; item.Title = task.Title;
            item.Status = static_cast<TaskProjectionStatus>(task.Status); item.Priority = static_cast<TaskProjectionPriority>(task.Priority);
            item.Version = task.Version; item.ManualHold = task.ManualHold; item.UpdatedAtUnixMilliseconds = task.UpdatedAtUnixMilliseconds;
            item.Rank = task.Rank; item.ColumnRevision = task.ColumnRevision; item.ExecutionContextId = task.ExecutionContextId;
            item.AgentId = task.AgentId; item.AgentRevision = task.AgentRevision; item.AgentName = task.AgentName; item.Model = task.Model;
            item.Mode = static_cast<TaskProjectionExecutionMode>(task.Mode); item.RunId = task.RunId;
            if (task.RunState) item.RunState = static_cast<TaskProjectionRunState>(*task.RunState);
            item.RunSequence = task.RunSequence; item.LastEventSequence = task.LastEventSequence;
            item.HasPendingMessages = task.HasPendingMessages; item.HasPendingIntent = task.HasPendingIntent; item.ArchivedProject = task.ArchivedProject;
            page.Tasks.push_back(std::move(item));
        }
        return result;
    }

    namespace
    {
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardCommandResult> mapped(std::string commandId, const Microsoft::Terminal::TaskCore::TaskBoardCommandResult& stored)
        {
            RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardCommandResult> result;
            result.Status = stored.Status; result.Error = stored.Error;
            auto& value = result.Value;
            value.CommandId = std::move(commandId); value.TaskId = stored.Task.Value(); value.TaskVersion = stored.TaskVersion;
            value.ColumnRevision = stored.ColumnRevision; value.EventSequence = stored.EventSequence;
            if (stored.Run) value.RunId = stored.Run->Value();
            if (stored.Operation) value.OperationId = stored.Operation->Value();
            value.Pending = stored.Pending; value.Replayed = stored.Status == TaskStoreCommandStatus::Replayed;
            return result;
        }
    }

    RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardCommandResult> RuntimeTaskBoardService::Execute(const TaskBoardCommandRequest& request)
    {
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardCommandResult> result;
        if (!Valid()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = "Task board is unavailable"; return result; }
        const auto encoded = EncodeTaskBoardCommandRequest(request);
        if (!encoded.Succeeded) { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = encoded.Error; return result; }
        const auto digest = RuntimeSha256Hex(std::span<const uint8_t>{ reinterpret_cast<const uint8_t*>(encoded.Payload.data()), encoded.Payload.size() });
        const auto event = NewRuntimeUuid(), run = NewRuntimeUuid(), message = NewRuntimeUuid(), intent = NewRuntimeUuid();
        if (!digest || !event || !run || !message || !intent) { result.Error = "Board command metadata could not be created"; return result; }
        TaskBoardCommand command;
        command.Metadata.Command = CommandId{ request.CommandId }; command.Metadata.Event = TaskEventId{ *event };
        command.Metadata.RequestDigest = *digest; command.Metadata.ExpectedTaskVersion = request.ExpectedTaskVersion;
        command.Metadata.TimestampUnixMilliseconds = RuntimeUnixMilliseconds();
        command.Metadata.EventPayloadJson = "{\"source\":\"task_board\",\"operation\":" + std::to_string(static_cast<uint8_t>(request.Operation)) + "}";
        command.Task = TaskId{ request.TaskId }; command.Workspace = WorkspaceId{ request.ProjectId };
        command.Effect = static_cast<TaskBoardEffect>(request.Operation); command.Target = static_cast<TaskStatus>(request.Target);
        command.ExpectedColumnRevision = request.ExpectedColumnRevision; command.Placement = static_cast<BoardPlacement>(request.Placement);
        if (request.AnchorId) command.Anchor = TaskId{ *request.AnchorId };
        if (request.RelatedRunId) command.RelatedRun = TaskRunId{ *request.RelatedRunId };
        command.Text = request.Text; command.Acceptance = request.Acceptance;
        if (request.UndoOfCommandId) command.UndoOf = CommandId{ *request.UndoOfCommandId };
        command.NewRun = TaskRunId{ *run }; command.NewMessage = TaskMessageId{ *message }; command.Intent = TaskIntentId{ *intent };
        result = mapped(request.CommandId, _store->ExecuteTaskBoardCommand(command));
        if (result.Succeeded() && _orchestrator) _orchestrator->Notify();
        return result;
    }

    RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardCommandResult> RuntimeTaskBoardService::QueryOperation(const TaskBoardOperationQueryRequest& request)
    {
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardCommandResult> result;
        if (!Valid()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = "Task board is unavailable"; return result; }
        const auto checked = EncodeTaskBoardOperationQueryRequest(request);
        if (!checked.Succeeded) { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = checked.Error; return result; }
        return mapped(request.OperationId, _store->QueryTaskBoardOperation(CommandId{ request.OperationId }));
    }
}
