// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProjectionService.h"

#include "RuntimeCrypto.h"

#include "TaskRunStatePolicy.h"
#include "TaskStatusPolicy.h"

#include <json/json.h>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskProvider;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        int64_t _unixMilliseconds() noexcept
        {
            FILETIME fileTime{};
            GetSystemTimeAsFileTime(&fileTime);
            ULARGE_INTEGER ticks{};
            ticks.LowPart = fileTime.dwLowDateTime;
            ticks.HighPart = fileTime.dwHighDateTime;
            constexpr uint64_t epoch{ 116444736000000000ull };
            return ticks.QuadPart < epoch ? 0 : static_cast<int64_t>((ticks.QuadPart - epoch) / 10'000);
        }

        RuntimeTaskProjectionStatus _status(const TaskStoreCommandStatus value) noexcept
        {
            switch (value)
            {
            case TaskStoreCommandStatus::Succeeded: return RuntimeTaskProjectionStatus::Succeeded;
            case TaskStoreCommandStatus::Replayed: return RuntimeTaskProjectionStatus::Replayed;
            case TaskStoreCommandStatus::NotFound:
            case TaskStoreCommandStatus::Tombstoned: return RuntimeTaskProjectionStatus::NotFound;
            case TaskStoreCommandStatus::VersionConflict:
            case TaskStoreCommandStatus::IdempotencyConflict:
            case TaskStoreCommandStatus::InvalidTransition:
            case TaskStoreCommandStatus::ActiveRunExists:
            case TaskStoreCommandStatus::ActiveIntentExists: return RuntimeTaskProjectionStatus::Conflict;
            case TaskStoreCommandStatus::NotReady: return RuntimeTaskProjectionStatus::StoreUnavailable;
            case TaskStoreCommandStatus::InvalidCommand:
            case TaskStoreCommandStatus::InvalidRecord:
            case TaskStoreCommandStatus::CapacityExceeded: return RuntimeTaskProjectionStatus::InvalidRequest;
            default: return RuntimeTaskProjectionStatus::ResourceFailure;
            }
        }

        RuntimeTaskProjectionCommandResult _failure(
            const RuntimeTaskProjectionStatus status,
            std::string error)
        {
            RuntimeTaskProjectionCommandResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }

        std::optional<std::wstring> _wide(const std::string_view value)
        {
            if (value.empty() || value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return std::nullopt;
            const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (count <= 0) return std::nullopt;
            std::wstring result(static_cast<size_t>(count), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count) != count) return std::nullopt;
            return result;
        }

        std::optional<std::string> _utf8(const std::wstring_view value)
        {
            if (value.empty()) return std::string{};
            if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return std::nullopt;
            const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0) return std::nullopt;
            std::string result(static_cast<size_t>(count), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) != count) return std::nullopt;
            return result;
        }

        std::string _eventPayload(const TaskUiCommandRequest& request)
        {
            Json::Value payload{ Json::objectValue };
            payload["operation"] = std::string{ TaskUiCommandOperationName(request.Operation) };
            payload["source"] = "terminal_tasks_page";
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            return Json::writeString(builder, payload);
        }

        bool _metadata(
            TaskCommandMetadata& metadata,
            const TaskUiCommandRequest& request,
            const uint64_t expectedVersion,
            const int64_t minimumTimestamp,
            std::string receipt,
            std::string& error)
        {
            const auto command = ParseId<CommandId>(request.CommandId);
            const auto event = NewRuntimeUuid();
            const auto encoded = EncodeTaskUiCommandRequest(request);
            const auto digest = encoded.Succeeded ? RuntimeSha256Hex(std::span<const uint8_t>{
                reinterpret_cast<const uint8_t*>(encoded.Payload.data()), encoded.Payload.size() }) : std::nullopt;
            if (!command || !event || !digest)
            {
                error = "Runtime Task command metadata could not be created";
                return false;
            }
            metadata.Command = *command;
            metadata.Event = TaskEventId{ *event };
            metadata.ExpectedTaskVersion = expectedVersion;
            metadata.RequestDigest = *digest;
            metadata.EventPayloadJson = _eventPayload(request);
            metadata.ReceiptResultJson = std::move(receipt);
            metadata.TimestampUnixMilliseconds = (std::max)(_unixMilliseconds(), minimumTimestamp);
            return true;
        }

        std::optional<std::string> _receipt(const TaskUiCommandResult& result)
        {
            const auto encoded = EncodeTaskUiCommandResult(result);
            return encoded.Succeeded ? std::optional<std::string>{ encoded.Payload } : std::nullopt;
        }

        RuntimeTaskProjectionCommandResult _stored(
            const TaskStoreCommandResult& stored,
            TaskUiCommandResult anticipated)
        {
            RuntimeTaskProjectionCommandResult result;
            result.Status = _status(stored.Status);
            result.Error = stored.Error;
            if (stored.Status == TaskStoreCommandStatus::Replayed)
            {
                const auto decoded = DecodeTaskUiCommandResult(stored.ResultJson);
                if (!decoded.Succeeded()) return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Stored Task command receipt is invalid");
                result.Result = decoded.Value;
                result.Result.Replayed = true;
            }
            else if (stored.Succeeded())
            {
                anticipated.TaskVersion = stored.CommittedTaskVersion;
                anticipated.EventSequence = stored.LastEventSequence;
                result.Result = std::move(anticipated);
            }
            return result;
        }

        const TaskRunRecord* _activeRun(const TaskAggregateSnapshot& aggregate) noexcept
        {
            const auto found = std::ranges::find_if(aggregate.Runs, [](const auto& run) { return IsTaskRunActive(run.State); });
            return found == aggregate.Runs.end() ? nullptr : &*found;
        }
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::Execute(
        const TaskUiCommandRequest& request,
        const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        std::string error;
        if (!Valid() || !ValidateTaskUiCommand(request, error))
        {
            return _failure(RuntimeTaskProjectionStatus::InvalidRequest, error.empty() ? "Runtime Task projection service is invalid" : std::move(error));
        }
        RuntimeTaskProjectionCommandResult result;
        switch (request.Operation)
        {
        case TaskUiCommandOperation::Create: result = _create(request); break;
        case TaskUiCommandOperation::Start: result = _start(request); break;
        case TaskUiCommandOperation::Stop: result = _stop(request, cancellationEvent); break;
        case TaskUiCommandOperation::Close: result = _close(request); break;
        case TaskUiCommandOperation::MarkDone:
        case TaskUiCommandOperation::Reopen: result = _status(request); break;
        default: result = _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task command operation is unsupported"); break;
        }
        if (result.Succeeded())
        {
            _generation = _generation == (std::numeric_limits<uint64_t>::max)() ? 1 : _generation + 1;
        }
        return result;
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_create(const TaskUiCommandRequest& request)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        const auto workspaceId = request.Draft ? ParseId<WorkspaceId>(request.Draft->WorkspaceId) : std::nullopt;
        const auto source = request.Draft ? _wide(request.Draft->SourceRoot) : std::nullopt;
        if (!taskId || !workspaceId || !source || !std::filesystem::path{ *source }.is_absolute())
        {
            return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task create path or identity is invalid");
        }
        try
        {
            const auto contextId = NewRuntimeUuid();
            const auto terminalId = NewRuntimeUuid();
            if (!contextId || !terminalId) return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task aggregate identity generation failed");
            const auto now = _unixMilliseconds();
            const auto& draft = *request.Draft;
            CreateTaskCommand command;
            command.Task.Id = *taskId;
            command.Task.Workspace = *workspaceId;
            command.Task.Title = draft.Title;
            command.Task.Description = draft.Description;
            for (size_t index = 0; index < draft.Acceptance.size(); ++index)
            {
                command.Task.AcceptanceCriteria.push_back({ static_cast<uint32_t>(index), draft.Acceptance[index], false });
            }
            command.Task.Status = TaskStatus::Todo;
            command.Task.Priority = static_cast<TaskPriority>(draft.Priority);
            command.Task.Version = 1;
            command.Task.CreatedAtUnixMilliseconds = now;
            command.Task.UpdatedAtUnixMilliseconds = now;
            command.ExecutionContext.Id = ExecutionContextId{ *contextId };
            command.ExecutionContext.Task = *taskId;
            command.ExecutionContext.Revision = 1;
            command.ExecutionContext.Mode = static_cast<ExecutionMode>(draft.ExecutionMode);
            command.ExecutionContext.SourceRoot = draft.SourceRoot;
            command.ExecutionContext.RelativeWorkingDirectory = draft.RelativeWorkingDirectory;
            command.ExecutionContext.Provider = draft.Provider;
            command.ExecutionContext.Model = draft.Model;
            command.ExecutionContext.Instructions = draft.Instructions;
            command.ExecutionContext.CreatedAtUnixMilliseconds = now;
            if (command.ExecutionContext.Mode == ExecutionMode::Direct)
            {
                command.ExecutionContext.RuntimeRoot = draft.SourceRoot;
            }
            else
            {
                const auto taskPath = _wide(taskId->Value());
                if (!taskPath) return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task Worktree identity is not valid UTF-8");
                const auto runtimePath = _runtimeDataRoot / L"task-worktrees" / *taskPath;
                const auto runtimeUtf8 = _utf8(runtimePath.native());
                const auto commonUtf8 = _utf8((std::filesystem::path{ *source } / L".git").native());
                if (!runtimeUtf8 || !commonUtf8) return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task Worktree path is not valid UTF-8");
                command.ExecutionContext.RuntimeRoot = *runtimeUtf8;
                command.ExecutionContext.RepositoryCommonDirectory = *commonUtf8;
                command.ExecutionContext.WorktreePath = *runtimeUtf8;
                command.ExecutionContext.Branch = "refs/heads/wt-task/" + taskId->Value();
            }
            command.Terminal.Id = TaskTerminalId{ *terminalId };
            command.Terminal.Task = *taskId;
            command.Terminal.State = TaskTerminalState::NotCreated;
            command.Terminal.UpdatedAtUnixMilliseconds = now;
            TaskUiCommandResult anticipated;
            anticipated.CommandId = request.CommandId;
            anticipated.TaskId = request.TaskId;
            anticipated.TaskVersion = 1;
            anticipated.EventSequence = 1;
            const auto receipt = _receipt(anticipated);
            std::string error;
            if (!receipt || !_metadata(command.Metadata, request, 0, now, *receipt, error))
            {
                return _failure(RuntimeTaskProjectionStatus::ResourceFailure, error.empty() ? "Task create receipt could not be encoded" : std::move(error));
            }
            return _stored(_store->CreateTask(command), std::move(anticipated));
        }
        catch (...)
        {
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task creation exhausted resources");
        }
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_start(const TaskUiCommandRequest& request)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        if (!taskId) return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task start identity is invalid");
        const auto aggregate = _store->LoadTask(*taskId);
        if (!aggregate) return _failure(RuntimeTaskProjectionStatus::NotFound, "Task was not found");
        if (aggregate->ExecutionContexts.empty()) return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task execution context is unavailable");
        const auto runId = NewRuntimeUuid();
        if (!runId) return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "TaskRun identity generation failed");
        CreateTaskRunCommand command;
        command.Run.Id = TaskRunId{ *runId };
        command.Run.Task = *taskId;
        command.Run.ExecutionContext = aggregate->ExecutionContexts.back().Id;
        command.Run.Trigger = TaskRunTrigger::Initial;
        command.Run.State = TaskRunState::Queued;
        command.Run.CreatedAtUnixMilliseconds = (std::max)(_unixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
        TaskUiCommandResult anticipated;
        anticipated.CommandId = request.CommandId;
        anticipated.TaskId = request.TaskId;
        anticipated.RunId = *runId;
        anticipated.TaskVersion = request.ExpectedTaskVersion + 1;
        anticipated.EventSequence = aggregate->LastEventSequence + 1;
        anticipated.Pending = true;
        const auto receipt = _receipt(anticipated);
        std::string error;
        if (!receipt || !_metadata(command.Metadata, request, request.ExpectedTaskVersion, command.Run.CreatedAtUnixMilliseconds, *receipt, error))
        {
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, error.empty() ? "Task start receipt could not be encoded" : std::move(error));
        }
        return _stored(_store->CreateTaskRun(command), std::move(anticipated));
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_stop(
        const TaskUiCommandRequest& request,
        const HANDLE cancellationEvent)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        if (!taskId) return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task stop identity is invalid");
        const auto aggregate = _store->LoadTask(*taskId);
        if (!aggregate) return _failure(RuntimeTaskProjectionStatus::NotFound, "Task was not found");
        const auto run = _activeRun(*aggregate);
        if (!run) return _failure(RuntimeTaskProjectionStatus::Conflict, "Task has no active Run");
        if (run->State == TaskRunState::Running && _provider)
        {
            const auto interrupted = _provider->InterruptRun(request.TaskId, run->Id.Value(), cancellationEvent);
            if (interrupted.Succeeded())
            {
                RuntimeTaskProjectionCommandResult result;
                result.Status = RuntimeTaskProjectionStatus::Succeeded;
                result.Result = { request.CommandId, request.TaskId, run->Id.Value(), aggregate->Task.Version, aggregate->LastEventSequence, false, true };
                return result;
            }
        }
        ChangeTaskRunStateCommand command;
        command.Task = *taskId;
        command.Run = run->Id;
        command.RequestedState = TaskRunState::Cancelled;
        command.ReasonCode = "user-stop";
        TaskUiCommandResult anticipated{ request.CommandId, request.TaskId, run->Id.Value(), aggregate->Task.Version + 1, aggregate->LastEventSequence + 1 };
        const auto receipt = _receipt(anticipated);
        std::string error;
        if (!receipt || !_metadata(command.Metadata, request, request.ExpectedTaskVersion, aggregate->Task.UpdatedAtUnixMilliseconds, *receipt, error))
        {
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task stop metadata could not be created");
        }
        return _stored(_store->ChangeTaskRunState(command), std::move(anticipated));
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_close(const TaskUiCommandRequest& request)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        if (!taskId) return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task close identity is invalid");
        const auto aggregate = _store->LoadTask(*taskId);
        if (!aggregate) return _failure(RuntimeTaskProjectionStatus::NotFound, "Task was not found");
        if (_activeRun(*aggregate)) return _failure(RuntimeTaskProjectionStatus::Conflict, "Stop the active TaskRun before closing its terminal");
        ChangeTaskTerminalStateCommand command;
        command.Task = *taskId;
        command.RequestedState = TaskTerminalState::Closed;
        command.RequestedGeneration = aggregate->Terminal.Generation;
        TaskUiCommandResult anticipated{ request.CommandId, request.TaskId, std::nullopt, aggregate->Task.Version + 1, aggregate->LastEventSequence + 1 };
        const auto receipt = _receipt(anticipated);
        std::string error;
        if (!receipt || !_metadata(command.Metadata, request, request.ExpectedTaskVersion, aggregate->Task.UpdatedAtUnixMilliseconds, *receipt, error))
        {
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task close metadata could not be created");
        }
        return _stored(_store->ChangeTaskTerminalState(command), std::move(anticipated));
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_status(const TaskUiCommandRequest& request)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        if (!taskId) return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task status identity is invalid");
        const auto aggregate = _store->LoadTask(*taskId);
        if (!aggregate) return _failure(RuntimeTaskProjectionStatus::NotFound, "Task was not found");
        ChangeTaskStatusCommand command;
        command.Task = *taskId;
        command.RequestedStatus = request.Operation == TaskUiCommandOperation::MarkDone ? TaskStatus::Done : TaskStatus::Todo;
        command.Authority = TaskStatusAuthority::User;
        TaskUiCommandResult anticipated{ request.CommandId, request.TaskId, std::nullopt, aggregate->Task.Version + 1, aggregate->LastEventSequence + 1 };
        const auto receipt = _receipt(anticipated);
        std::string error;
        if (!receipt || !_metadata(command.Metadata, request, request.ExpectedTaskVersion, aggregate->Task.UpdatedAtUnixMilliseconds, *receipt, error))
        {
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task status metadata could not be created");
        }
        return _stored(_store->ChangeTaskStatus(command), std::move(anticipated));
    }
}
