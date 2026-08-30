// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreCommandValidation.h"

#include "TaskRecordValidation.h"
#include "TaskRunStatePolicy.h"

#include <limits>
#include <type_traits>

namespace Microsoft::Terminal::TaskCore::details
{
    namespace
    {
        bool _isLowerHexDigest(const std::string_view value) noexcept
        {
            if (value.size() != TaskStoreDigestCharacters)
            {
                return false;
            }
            return std::ranges::all_of(value, [](const char character) {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            });
        }

        bool _validOptionalId(const auto& value) noexcept
        {
            return !value || IsCanonicalUuid(value->Value());
        }

        template<typename TEnum>
        bool _enumInRange(const TEnum value, const TEnum maximum) noexcept
        {
            return static_cast<std::underlying_type_t<TEnum>>(value) <=
                   static_cast<std::underlying_type_t<TEnum>>(maximum);
        }
    }

    bool ValidateCommandMetadata(const TaskCommandMetadata& metadata, std::string& error)
    {
        if (!IsCanonicalUuid(metadata.Command.Value()) || !IsCanonicalUuid(metadata.Event.Value()))
        {
            error = "Command and event identifiers must be canonical UUIDs";
            return false;
        }
        if (!_isLowerHexDigest(metadata.RequestDigest))
        {
            error = "Request digest must be 64 lowercase hexadecimal characters";
            return false;
        }
        if (metadata.EventPayloadJson.size() > MaximumTaskStoreJsonBytes || metadata.ReceiptResultJson.size() > MaximumTaskStoreJsonBytes)
        {
            error = "Command JSON payload exceeds its bound";
            return false;
        }
        if (metadata.TimestampUnixMilliseconds < 0)
        {
            error = "Command timestamp is invalid";
            return false;
        }
        if (metadata.ExpectedTaskVersion > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            error = "Expected Task version exceeds the storage range";
            return false;
        }
        return true;
    }

    bool ValidateCreateTaskCommand(const CreateTaskCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto taskIssues = ValidateTaskRecord(command.Task);
        const auto contextIssues = ValidateExecutionContextRecord(command.ExecutionContext);
        if (!taskIssues.empty() || !contextIssues.empty())
        {
            error = !taskIssues.empty() ? taskIssues.front().Field : contextIssues.front().Field;
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion != 0 || command.Task.Version != 1 ||
            command.Task.Status != TaskStatus::Todo ||
            !_enumInRange(command.Task.Priority, TaskPriority::High) ||
            (command.Task.ForkedFromTaskId && command.Task.ForkedFromTaskId.value() == command.Task.Id) ||
            command.ExecutionContext.Task != command.Task.Id || command.ExecutionContext.Revision != 1 ||
            command.Terminal.Task != command.Task.Id || !IsCanonicalUuid(command.Terminal.Id.Value()) ||
            command.Terminal.State != TaskTerminalState::NotCreated || command.Terminal.Generation != 0 ||
            command.Terminal.CurrentAgentSessionId || command.Terminal.UpdatedAtUnixMilliseconds < 0 ||
            command.ExecutionContext.CreatedAtUnixMilliseconds < command.Task.CreatedAtUnixMilliseconds ||
            command.Terminal.UpdatedAtUnixMilliseconds < command.Task.CreatedAtUnixMilliseconds ||
            command.Task.UpdatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds ||
            command.ExecutionContext.CreatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds ||
            command.Terminal.UpdatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds)
        {
            error = "Create Task aggregate identity or initial state is inconsistent";
            return false;
        }
        return true;
    }

    bool ValidateChangeTaskStatusCommand(const ChangeTaskStatusCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 || !IsCanonicalUuid(command.Task.Value()) ||
            !_enumInRange(command.RequestedStatus, TaskStatus::Cancelled) ||
            command.Authority < TaskStatusAuthority::User || command.Authority > TaskStatusAuthority::SystemRecovery)
        {
            error = "Task status transition command is invalid";
            return false;
        }
        return true;
    }

    bool ValidateCreateTaskRunCommand(const CreateTaskRunCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto& run = command.Run;
        const auto retryTrigger = run.Trigger == TaskRunTrigger::ManualRetry ||
                                  run.Trigger == TaskRunTrigger::AutomaticRetry;
        if (!IsCanonicalUuid(run.Id.Value()) || !IsCanonicalUuid(run.Task.Value()) ||
            !IsCanonicalUuid(run.ExecutionContext.Value()) || !_validOptionalId(run.AgentSession) ||
            !_validOptionalId(run.RetryOfRunId) || !_enumInRange(run.Trigger, TaskRunTrigger::FollowUp) ||
            retryTrigger != run.RetryOfRunId.has_value() ||
            run.State != TaskRunState::Queued || command.Metadata.ExpectedTaskVersion == 0 ||
            run.Sequence != 0 || run.CreatedAtUnixMilliseconds < 0 || run.StartedAtUnixMilliseconds ||
            run.FinishedAtUnixMilliseconds || !run.Result.empty() || !run.ReasonCode.empty() ||
            run.CreatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds)
        {
            error = "New TaskRun is not in its canonical queued state";
            return false;
        }
        return true;
    }

    bool ValidateRunStateCommand(const ChangeTaskRunStateCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error) ||
            !IsCanonicalUuid(command.Task.Value()) || !IsCanonicalUuid(command.Run.Value()) ||
            command.Metadata.ExpectedTaskVersion == 0 ||
            !_enumInRange(command.RequestedState, TaskRunState::Interrupted))
        {
            if (error.empty())
            {
                error = "TaskRun transition identifiers are invalid";
            }
            return false;
        }
        if (command.Result.size() > MaximumTaskRunResultBytes || command.ReasonCode.size() > MaximumTaskReasonCodeBytes)
        {
            error = "TaskRun outcome payload exceeds its bound";
            return false;
        }
        if (!IsTaskRunTerminal(command.RequestedState) && (!command.Result.empty() || !command.ReasonCode.empty()))
        {
            error = "Only terminal TaskRun states may carry an outcome";
            return false;
        }
        return true;
    }

    bool ValidateTerminalStateCommand(const ChangeTaskTerminalStateCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 || !IsCanonicalUuid(command.Task.Value()) ||
            !_enumInRange(command.RequestedState, TaskTerminalState::Closed) ||
            command.RequestedGeneration > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            error = "TaskTerminal transition command is invalid";
            return false;
        }
        return true;
    }
}
