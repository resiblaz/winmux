// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProjectionService.h"
#include "RuntimeTaskOrchestrator.h"

#include "TaskRunStatePolicy.h"
#include <json/json.h>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        TaskProjectionRecord _projection(
            const TaskAggregateSnapshot& aggregate,
            std::vector<TaskEventRecord> events,
            const size_t eventLimit)
        {
            TaskProjectionRecord result;
            result.Id = aggregate.Task.Id.Value();
            result.WorkspaceId = aggregate.Task.Workspace.Value();
            result.Title = aggregate.Task.Title;
            result.Description = aggregate.Task.Description;
            for (const auto& item : aggregate.Task.AcceptanceCriteria)
            {
                result.Acceptance.push_back({ item.Text, item.Satisfied });
            }
            result.Status = static_cast<TaskProjectionStatus>(aggregate.Task.Status);
            result.Priority = static_cast<TaskProjectionPriority>(aggregate.Task.Priority);
            result.Version = aggregate.Task.Version;
            result.ManualHold = aggregate.Task.ManualHold;
            result.CreatedAtUnixMilliseconds = aggregate.Task.CreatedAtUnixMilliseconds;
            result.UpdatedAtUnixMilliseconds = aggregate.Task.UpdatedAtUnixMilliseconds;
            if (!aggregate.ExecutionContexts.empty())
            {
                const auto context = std::ranges::max_element(
                    aggregate.ExecutionContexts, {}, &ExecutionContextRecord::Revision);
                result.Execution.Id = context->Id.Value();
                result.Execution.Revision = context->Revision;
                result.Execution.Mode = static_cast<TaskProjectionExecutionMode>(context->Mode);
                result.Execution.SourceRoot = context->SourceRoot;
                result.Execution.RuntimeRoot = context->RuntimeRoot;
                result.Execution.WorktreePath = context->WorktreePath;
                result.Execution.Branch = context->Branch;
                result.Execution.RelativeWorkingDirectory = context->RelativeWorkingDirectory;
                result.Execution.Provider = context->Provider;
                result.Execution.Model = context->Model;
                result.Execution.Instructions = context->Instructions;
                if (context->SourceAgentId) result.Execution.Agent = AgentProfileReference{ context->SourceAgentId->Value(), context->SourceAgentRevision };
                result.Execution.AgentName = context->AgentNameSnapshot;
            }
            result.Terminal.State = static_cast<TaskProjectionTerminalState>(aggregate.Terminal.State);
            result.Terminal.Generation = aggregate.Terminal.Generation;
            if (aggregate.Terminal.CurrentAgentSessionId)
            {
                result.Terminal.CurrentAgentSessionId = aggregate.Terminal.CurrentAgentSessionId->Value();
            }
            for (const auto& session : aggregate.AgentSessions)
            {
                result.AgentSessions.push_back({
                    session.Id.Value(), session.Provider, session.ProviderSessionId,
                    static_cast<uint8_t>(session.State), session.IsCurrent,
                });
            }
            for (const auto& run : aggregate.Runs)
            {
                TaskProjectionRunRecord projected;
                projected.Id = run.Id.Value();
                projected.ExecutionContextId = run.ExecutionContext.Value();
                if (run.AgentSession) projected.AgentSessionId = run.AgentSession->Value();
                if (run.RetryOfRunId) projected.RetryOfRunId = run.RetryOfRunId->Value();
                projected.Trigger = static_cast<uint8_t>(run.Trigger);
                projected.State = static_cast<TaskProjectionRunState>(run.State);
                projected.Sequence = run.Sequence;
                projected.CreatedAtUnixMilliseconds = run.CreatedAtUnixMilliseconds;
                projected.StartedAtUnixMilliseconds = run.StartedAtUnixMilliseconds;
                projected.FinishedAtUnixMilliseconds = run.FinishedAtUnixMilliseconds;
                projected.Result = run.Result;
                projected.ReasonCode = run.ReasonCode;
                if (const auto context = std::ranges::find(aggregate.ExecutionContexts, run.ExecutionContext, &ExecutionContextRecord::Id);
                    context != aggregate.ExecutionContexts.end())
                {
                    if (context->SourceAgentId) projected.Agent = AgentProfileReference{ context->SourceAgentId->Value(), context->SourceAgentRevision };
                    projected.AgentName = context->AgentNameSnapshot;
                    projected.Provider = context->Provider;
                    projected.Model = context->Model;
                    projected.ExecutionRevision = context->Revision;
                }
                result.Runs.emplace_back(std::move(projected));
            }
            std::ranges::sort(result.Runs, [](const auto& left, const auto& right) {
                const auto leftActive = IsTaskRunActive(static_cast<TaskRunState>(left.State));
                const auto rightActive = IsTaskRunActive(static_cast<TaskRunState>(right.State));
                return leftActive != rightActive ? leftActive : left.Sequence > right.Sequence;
            });
            for (const auto& message : aggregate.Messages)
            {
                TaskProjectionMessageRecord projected;
                projected.Id = message.Id.Value();
                projected.Author = static_cast<uint8_t>(message.Author);
                projected.State = static_cast<uint8_t>(message.State);
                projected.Revision = message.Revision;
                projected.Content = message.Content;
                if (message.ClaimedByRunId) projected.ClaimedByRunId = message.ClaimedByRunId->Value();
                projected.CreatedAtUnixMilliseconds = message.CreatedAtUnixMilliseconds;
                projected.UpdatedAtUnixMilliseconds = message.UpdatedAtUnixMilliseconds;
                result.Messages.emplace_back(std::move(projected));
            }
            for (const auto& event : events)
            {
                TaskProjectionEventRecord projected;
                projected.Id = event.Id.Value();
                if (event.RunId) projected.RunId = event.RunId->Value();
                projected.AggregateSequence = event.AggregateSequence;
                projected.RunSequence = event.RunSequence;
                projected.Type = event.Type;
                projected.PayloadJson = event.PayloadJson;
                projected.CreatedAtUnixMilliseconds = event.CreatedAtUnixMilliseconds;
                result.Events.emplace_back(std::move(projected));
            }
            // Include a current cleanup failure even when the event history is
            // limited, so the retained worktree has an actionable explanation.
            if (eventLimit != 0 && aggregate.Intent && aggregate.Intent->Kind == TaskIntentKind::WorktreeCleanup &&
                aggregate.Intent->Stage == TaskIntentStage::Failed)
            {
                Json::Value payload{ Json::objectValue };
                payload["reasonCode"] = aggregate.Intent->ReasonCode;
                Json::StreamWriterBuilder writer;
                writer["indentation"] = "";
                if (result.Events.size() >= eventLimit)
                    result.Events.erase(result.Events.begin());
                result.Events.push_back({ aggregate.Intent->Id.Value(), std::nullopt,
                                          aggregate.LastEventSequence, std::nullopt,
                                          "TaskWorktreeCleanupFailed", Json::writeString(writer, payload),
                                          aggregate.Intent->UpdatedAtUnixMilliseconds });
            }
            result.LastEventSequence = aggregate.LastEventSequence;
            return result;
        }
    }

    RuntimeTaskProjectionService::RuntimeTaskProjectionService(
        std::shared_ptr<ITaskStore> store,
        std::shared_ptr<RuntimeTaskProviderService> provider,
        std::filesystem::path runtimeDataRoot,
        std::shared_ptr<RuntimeTaskTerminalService> terminals,
        std::shared_ptr<RuntimeTaskOrchestrator> orchestrator) noexcept :
        _store{ std::move(store) },
        _provider{ std::move(provider) },
        _terminals{ std::move(terminals) },
        _orchestrator{ std::move(orchestrator) },
        _runtimeDataRoot{ std::move(runtimeDataRoot) }
    {
    }

    bool RuntimeTaskProjectionService::Valid() const noexcept
    {
        return _store && _store->IsReady() && !_runtimeDataRoot.empty();
    }

    RuntimeTaskProjectionQueryResult RuntimeTaskProjectionService::Query(
        const TaskProjectionQueryRequest& request)
    {
        const std::scoped_lock lock{ _mutex };
        RuntimeTaskProjectionQueryResult result;
        std::string error;
        if (!Valid() || !ValidateTaskProjectionQuery(request, error))
        {
            result.Status = RuntimeTaskProjectionStatus::InvalidRequest;
            result.Error = error.empty() ? "Runtime Task projection service is invalid" : std::move(error);
            return result;
        }
        try
        {
            std::vector<TaskRecord> tasks;
            if (request.TaskId)
            {
                const auto id = ParseId<TaskId>(*request.TaskId);
                if (!id)
                {
                    result.Status = RuntimeTaskProjectionStatus::InvalidRequest;
                    result.Error = "Task query identity is invalid";
                    return result;
                }
                const auto aggregate = _store->LoadTask(*id);
                if (!aggregate)
                {
                    result.Status = RuntimeTaskProjectionStatus::NotFound;
                    result.Error = "Task was not found";
                    return result;
                }
                tasks.push_back(aggregate->Task);
            }
            else
            {
                std::optional<WorkspaceId> workspace;
                if (request.WorkspaceId) workspace = WorkspaceId{ *request.WorkspaceId };
                tasks = _store->ListTasks(workspace);
            }
            result.Projection.Generation = _generation;
            _generation = _generation == (std::numeric_limits<uint64_t>::max)() ? 1 : _generation + 1;
            for (const auto& task : tasks)
            {
                if (result.Projection.Tasks.size() >= MaximumTaskProjectionItems) break;
                if (!request.IncludeCompleted &&
                    (task.Status == TaskStatus::Done || task.Status == TaskStatus::Cancelled)) continue;
                const auto aggregate = _store->LoadTask(task.Id);
                if (!aggregate) continue;
                const auto after = request.EventLimit != 0 && aggregate->LastEventSequence > request.EventLimit ?
                                       aggregate->LastEventSequence - request.EventLimit : 0;
                auto events = request.EventLimit == 0 ? std::vector<TaskEventRecord>{} :
                                                       _store->ListEvents(task.Id, after, request.EventLimit);
                result.Projection.Tasks.emplace_back(_projection(*aggregate, std::move(events), request.EventLimit));
            }
            result.Status = RuntimeTaskProjectionStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            result.Status = RuntimeTaskProjectionStatus::ResourceFailure;
            result.Error = "Runtime Task projection query exhausted resources";
            return result;
        }
    }
}
