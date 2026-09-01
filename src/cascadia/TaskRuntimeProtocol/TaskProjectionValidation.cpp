// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProjectionRpc.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        bool _id(const std::string_view value) noexcept
        {
            return IsCanonicalProtocolUuid(value);
        }

        bool _text(const std::string_view value, const size_t maximum, const bool required = false) noexcept
        {
            return (!required || !value.empty()) && value.size() <= maximum &&
                   value.find('\0') == std::string_view::npos && IsValidUtf8(value);
        }

        bool _optionalId(const std::optional<std::string>& value) noexcept
        {
            return !value || _id(*value);
        }
    }

    bool ValidateTaskProjectionQuery(const TaskProjectionQueryRequest& value, std::string& error) noexcept
    {
        if ((value.TaskId && !_id(*value.TaskId)) ||
            (value.WorkspaceId && !_id(*value.WorkspaceId)) ||
            (value.TaskId && value.WorkspaceId) || value.EventLimit > MaximumTaskProjectionEvents)
        {
            error = "Task projection query is invalid";
            return false;
        }
        return true;
    }

    bool ValidateTaskProjection(const TaskProjectionRecord& value, std::string& error) noexcept
    {
        if (!_id(value.Id) || !_id(value.WorkspaceId) ||
            !_text(value.Title, 1024, true) || !_text(value.Description, 64 * 1024) ||
            value.Version == 0 || value.CreatedAtUnixMilliseconds < 0 ||
            value.UpdatedAtUnixMilliseconds < value.CreatedAtUnixMilliseconds ||
            value.Status > TaskProjectionStatus::Cancelled ||
            value.Priority > TaskProjectionPriority::High ||
            value.Acceptance.size() > MaximumTaskProjectionAcceptance ||
            value.AgentSessions.size() > MaximumTaskProjectionRuns ||
            value.Runs.size() > MaximumTaskProjectionRuns ||
            value.Messages.size() > MaximumTaskProjectionMessages ||
            value.Events.size() > MaximumTaskProjectionEvents ||
            !_id(value.Execution.Id) || value.Execution.Revision == 0 ||
            value.Execution.Mode > TaskProjectionExecutionMode::Worktree ||
            !_text(value.Execution.SourceRoot, 32 * 1024, true) ||
            !_text(value.Execution.RuntimeRoot, 32 * 1024, true) ||
            !_text(value.Execution.WorktreePath, 32 * 1024) ||
            !_text(value.Execution.Branch, 4 * 1024) ||
            !_text(value.Execution.RelativeWorkingDirectory, 32 * 1024) ||
            !_text(value.Execution.Provider, 256, true) ||
            !_text(value.Execution.Model, 256) || !_text(value.Execution.Instructions, 64 * 1024) ||
            value.Terminal.State > TaskProjectionTerminalState::Closed ||
            !_optionalId(value.Terminal.CurrentAgentSessionId))
        {
            error = "Task projection record is invalid";
            return false;
        }
        for (const auto& item : value.Acceptance)
        {
            if (!_text(item.Text, 8 * 1024, true))
            {
                error = "Task projection acceptance is invalid";
                return false;
            }
        }
        for (const auto& run : value.Runs)
        {
            if (!_id(run.Id) || !_id(run.ExecutionContextId) ||
                !_optionalId(run.AgentSessionId) || !_optionalId(run.RetryOfRunId) ||
                run.State > TaskProjectionRunState::Interrupted || run.Trigger > 3 ||
                run.Sequence == 0 || run.CreatedAtUnixMilliseconds < 0 ||
                !_text(run.Result, 256 * 1024) || !_text(run.ReasonCode, 256))
            {
                error = "Task projection Run is invalid";
                return false;
            }
        }
        for (const auto& session : value.AgentSessions)
        {
            if (!_id(session.Id) || !_text(session.Provider, 256, true) ||
                !_text(session.ProviderSessionId, 256, true) || session.State > 3)
            {
                error = "Task projection AgentSession is invalid";
                return false;
            }
        }
        for (const auto& message : value.Messages)
        {
            if (!_id(message.Id) || message.Author > 2 || message.State > 3 || message.Revision == 0 ||
                !_text(message.Content, 64 * 1024, true) || !_optionalId(message.ClaimedByRunId) ||
                message.CreatedAtUnixMilliseconds < 0 ||
                message.UpdatedAtUnixMilliseconds < message.CreatedAtUnixMilliseconds)
            {
                error = "Task projection message is invalid";
                return false;
            }
        }
        for (const auto& event : value.Events)
        {
            if (!_id(event.Id) || !_optionalId(event.RunId) || event.AggregateSequence == 0 ||
                !_text(event.Type, 128, true) || !_text(event.PayloadJson, 64 * 1024, true) ||
                event.CreatedAtUnixMilliseconds < 0)
            {
                error = "Task projection event is invalid";
                return false;
            }
        }
        return true;
    }

    std::string_view TaskUiCommandOperationName(const TaskUiCommandOperation value) noexcept
    {
        switch (value)
        {
        case TaskUiCommandOperation::Create: return "create";
        case TaskUiCommandOperation::Start: return "start";
        case TaskUiCommandOperation::Stop: return "stop";
        case TaskUiCommandOperation::Close: return "close";
        case TaskUiCommandOperation::MarkDone: return "mark_done";
        case TaskUiCommandOperation::Reopen: return "reopen";
        default: return {};
        }
    }

    std::optional<TaskUiCommandOperation> TaskUiCommandOperationFromName(const std::string_view value) noexcept
    {
        for (uint8_t candidate = 0; candidate < static_cast<uint8_t>(TaskUiCommandOperation::Count); ++candidate)
        {
            const auto operation = static_cast<TaskUiCommandOperation>(candidate);
            if (TaskUiCommandOperationName(operation) == value) return operation;
        }
        return std::nullopt;
    }

    bool ValidateTaskUiCommand(const TaskUiCommandRequest& value, std::string& error) noexcept
    {
        if (!_id(value.CommandId) || !_id(value.TaskId) ||
            value.Operation >= TaskUiCommandOperation::Count ||
            (value.Operation == TaskUiCommandOperation::Create) != value.Draft.has_value() ||
            (value.Operation == TaskUiCommandOperation::Create && value.ExpectedTaskVersion != 0) ||
            (value.Operation != TaskUiCommandOperation::Create && value.ExpectedTaskVersion == 0))
        {
            error = "Task UI command envelope is invalid";
            return false;
        }
        if (value.Draft)
        {
            const auto& draft = *value.Draft;
            if (!_id(draft.WorkspaceId) || !_text(draft.Title, 1024, true) ||
                !_text(draft.Description, 64 * 1024) || draft.Acceptance.size() > MaximumTaskProjectionAcceptance ||
                draft.Priority > TaskProjectionPriority::High || draft.ExecutionMode > TaskProjectionExecutionMode::Worktree ||
                !_text(draft.SourceRoot, 32 * 1024, true) || !_text(draft.RelativeWorkingDirectory, 32 * 1024) ||
                draft.Provider != "codex" || !_text(draft.Model, 256) || !_text(draft.Instructions, 64 * 1024))
            {
                error = "Task UI create draft is invalid";
                return false;
            }
            for (const auto& item : draft.Acceptance)
            {
                if (!_text(item, 8 * 1024, true))
                {
                    error = "Task UI acceptance criterion is invalid";
                    return false;
                }
            }
        }
        return true;
    }
}
