// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProjectionService.h"

#include "RuntimeCrypto.h"
#include "RuntimeTaskCommandMetadata.h"

#include "TaskRunStatePolicy.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        RuntimeTaskProjectionStatus _status(const TaskStoreCommandStatus status) noexcept
        {
            switch (status)
            {
            case TaskStoreCommandStatus::Succeeded:
                return RuntimeTaskProjectionStatus::Succeeded;
            case TaskStoreCommandStatus::Replayed:
                return RuntimeTaskProjectionStatus::Replayed;
            case TaskStoreCommandStatus::NotFound:
            case TaskStoreCommandStatus::Tombstoned:
                return RuntimeTaskProjectionStatus::NotFound;
            case TaskStoreCommandStatus::VersionConflict:
            case TaskStoreCommandStatus::IdempotencyConflict:
            case TaskStoreCommandStatus::InvalidTransition:
            case TaskStoreCommandStatus::ActiveRunExists:
            case TaskStoreCommandStatus::ActiveIntentExists:
                return RuntimeTaskProjectionStatus::Conflict;
            case TaskStoreCommandStatus::NotReady:
                return RuntimeTaskProjectionStatus::StoreUnavailable;
            case TaskStoreCommandStatus::InvalidCommand:
            case TaskStoreCommandStatus::InvalidRecord:
            case TaskStoreCommandStatus::CapacityExceeded:
                return RuntimeTaskProjectionStatus::InvalidRequest;
            default:
                return RuntimeTaskProjectionStatus::ResourceFailure;
            }
        }

        RuntimeTaskProjectionCommandResult _failure(RuntimeTaskProjectionStatus status, std::string error)
        {
            RuntimeTaskProjectionCommandResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }

        std::optional<std::string> _receipt(const TaskUiCommandResult& result)
        {
            const auto encoded = EncodeTaskUiCommandResult(result);
            return encoded.Succeeded ? std::optional<std::string>{ encoded.Payload } : std::nullopt;
        }

        std::optional<TaskCommandMetadata> _metadata(
            const TaskUiCommandRequest& request,
            const uint64_t expectedVersion,
            const int64_t timestamp,
            std::string receipt)
        {
            const auto command = ParseId<CommandId>(request.CommandId);
            const auto event = NewRuntimeUuid();
            const auto encoded = EncodeTaskUiCommandRequest(request);
            const auto digest = encoded.Succeeded ? RuntimeSha256Hex(std::span<const uint8_t>{
                                                        reinterpret_cast<const uint8_t*>(encoded.Payload.data()), encoded.Payload.size() }) :
                                                    std::nullopt;
            if (!command || !event || !digest || receipt.empty())
                return std::nullopt;
            TaskCommandMetadata result;
            result.Command = *command;
            result.Event = TaskEventId{ *event };
            result.ExpectedTaskVersion = expectedVersion;
            result.RequestDigest = *digest;
            result.EventPayloadJson = "{\"source\":\"terminal_tasks_page\"}";
            result.ReceiptResultJson = std::move(receipt);
            result.TimestampUnixMilliseconds = timestamp;
            return result;
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
                if (!decoded.Succeeded())
                    return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Stored Task command receipt is invalid");
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

        const TaskRunRecord* _active(const TaskAggregateSnapshot& aggregate) noexcept
        {
            const auto found = std::ranges::find_if(aggregate.Runs, [](const auto& run) { return IsTaskRunActive(run.State); });
            return found == aggregate.Runs.end() ? nullptr : &*found;
        }

        std::optional<std::wstring> _wide(const std::string_view value)
        {
            if (value.empty())
                return std::wstring{};
            const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (count <= 0)
                return std::nullopt;
            std::wstring result(static_cast<size_t>(count), L'\0');
            return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count) == count ?
                       std::optional<std::wstring>{ std::move(result) } :
                       std::nullopt;
        }

        std::optional<std::string> _utf8(const std::wstring_view value)
        {
            if (value.empty())
                return std::string{};
            const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0)
                return std::nullopt;
            std::string result(static_cast<size_t>(count), '\0');
            return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) == count ?
                       std::optional<std::string>{ std::move(result) } :
                       std::nullopt;
        }
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_message(const TaskUiCommandRequest& request)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        const auto aggregate = taskId ? _store->LoadTask(*taskId) : std::nullopt;
        if (!taskId || !aggregate)
            return _failure(RuntimeTaskProjectionStatus::NotFound, "Task was not found");
        TaskUiCommandResult anticipated{ request.CommandId, request.TaskId, std::nullopt, aggregate->Task.Version + 1, aggregate->LastEventSequence + 1 };
        const auto receipt = _receipt(anticipated);
        const auto metadata = receipt ? _metadata(
                                            request, request.ExpectedTaskVersion, (std::max)(RuntimeUnixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds), *receipt) :
                                        std::nullopt;
        if (!metadata)
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task message metadata could not be created");

        if (request.Operation == TaskUiCommandOperation::QueueMessage)
        {
            const auto messageId = NewRuntimeUuid();
            if (!messageId || !request.Text)
                return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task message is invalid");
            EnqueueTaskMessageCommand command;
            command.Metadata = *metadata;
            command.Message.Id = TaskMessageId{ *messageId };
            command.Message.Task = *taskId;
            command.Message.Author = TaskMessageAuthor::User;
            command.Message.State = TaskMessageState::Pending;
            command.Message.Revision = 1;
            command.Message.Content = *request.Text;
            command.Message.CreatedAtUnixMilliseconds = metadata->TimestampUnixMilliseconds;
            command.Message.UpdatedAtUnixMilliseconds = metadata->TimestampUnixMilliseconds;
            anticipated.Pending = true;
            command.Metadata.ReceiptResultJson = _receipt(anticipated).value_or(std::string{});
            return _stored(_store->EnqueueTaskMessage(command), std::move(anticipated));
        }

        const auto messageId = request.MessageId ? ParseId<TaskMessageId>(*request.MessageId) : std::nullopt;
        if (!messageId || !request.ExpectedMessageRevision)
            return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task message target is invalid");
        ReviseTaskMessageCommand command;
        command.Metadata = *metadata;
        command.Task = *taskId;
        command.Message = *messageId;
        command.ExpectedMessageRevision = *request.ExpectedMessageRevision;
        command.Remove = request.Operation == TaskUiCommandOperation::RemoveMessage;
        command.ReplacementContent = command.Remove ? std::nullopt : request.Text;
        return _stored(_store->ReviseTaskMessage(command), std::move(anticipated));
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_retry(const TaskUiCommandRequest& request)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        const auto retryId = request.RelatedRunId ? ParseId<TaskRunId>(*request.RelatedRunId) : std::nullopt;
        const auto aggregate = taskId ? _store->LoadTask(*taskId) : std::nullopt;
        if (!taskId || !retryId || !aggregate || aggregate->ExecutionContexts.empty())
        {
            return _failure(RuntimeTaskProjectionStatus::NotFound, "Task or retry Run was not found");
        }
        const auto prior = std::ranges::find(aggregate->Runs, *retryId, &TaskRunRecord::Id);
        if (prior == aggregate->Runs.end() || !IsTaskRunTerminal(prior->State) || _active(*aggregate))
        {
            return _failure(RuntimeTaskProjectionStatus::Conflict, "TaskRun is not eligible for retry");
        }
        const auto runId = NewRuntimeUuid();
        if (!runId)
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Retry identity generation failed");
        CreateTaskRunCommand command;
        command.Run.Id = TaskRunId{ *runId };
        command.Run.Task = *taskId;
        command.Run.ExecutionContext = prior->ExecutionContext;
        command.Run.RetryOfRunId = prior->Id;
        command.Run.Trigger = TaskRunTrigger::ManualRetry;
        command.Run.State = TaskRunState::Queued;
        command.Run.CreatedAtUnixMilliseconds = (std::max)(RuntimeUnixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
        command.ClearManualHold = true;
        TaskUiCommandResult anticipated{ request.CommandId, request.TaskId, *runId, aggregate->Task.Version + 1, aggregate->LastEventSequence + 1, false, true };
        const auto receipt = _receipt(anticipated);
        const auto metadata = receipt ? _metadata(request, request.ExpectedTaskVersion, command.Run.CreatedAtUnixMilliseconds, *receipt) : std::nullopt;
        if (!metadata)
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Retry metadata could not be created");
        command.Metadata = *metadata;
        return _stored(_store->CreateTaskRun(command), std::move(anticipated));
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_intent(const TaskUiCommandRequest& request)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        const auto aggregate = taskId ? _store->LoadTask(*taskId) : std::nullopt;
        if (!taskId || !aggregate)
            return _failure(RuntimeTaskProjectionStatus::NotFound, "Task was not found");
        const auto intentId = NewRuntimeUuid();
        if (!intentId)
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task intent identity generation failed");
        StartTaskIntentCommand command;
        command.Intent.Id = TaskIntentId{ *intentId };
        command.Intent.Task = *taskId;
        command.Intent.Kind = request.Operation == TaskUiCommandOperation::FinalizeWorktree ? TaskIntentKind::WorktreeCleanup :
                              request.Operation == TaskUiCommandOperation::Delete           ? TaskIntentKind::Delete :
                                                                                              TaskIntentKind::Close;
        command.Intent.Stage = TaskIntentStage::Requested;
        command.Intent.DetailJson = request.Operation == TaskUiCommandOperation::Cancel ? R"({"cancel":true,"source":"tasks_page"})" :
                                                                                          R"({"source":"tasks_page"})";
        command.Intent.CreatedAtUnixMilliseconds = (std::max)(RuntimeUnixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
        command.Intent.UpdatedAtUnixMilliseconds = command.Intent.CreatedAtUnixMilliseconds;
        TaskUiCommandResult anticipated{ request.CommandId, request.TaskId, std::nullopt, aggregate->Task.Version + 1, aggregate->LastEventSequence + 1, false, true };
        if (const auto active = _active(*aggregate))
            anticipated.RunId = active->Id.Value();
        const auto receipt = _receipt(anticipated);
        const auto metadata = receipt ? _metadata(request, request.ExpectedTaskVersion, command.Intent.CreatedAtUnixMilliseconds, *receipt) : std::nullopt;
        if (!metadata)
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task intent metadata could not be created");
        command.Metadata = *metadata;
        return _stored(_store->StartTaskIntent(command), std::move(anticipated));
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_derive(const TaskUiCommandRequest& request)
    {
        const auto sourceId = ParseId<TaskId>(request.TaskId);
        const auto derivedId = request.DerivedTaskId ? ParseId<TaskId>(*request.DerivedTaskId) : std::nullopt;
        const auto workspaceId = request.Draft ? ParseId<WorkspaceId>(request.Draft->WorkspaceId) : std::nullopt;
        const auto source = sourceId ? _store->LoadTask(*sourceId) : std::nullopt;
        const auto root = request.Draft ? _wide(request.Draft->SourceRoot) : std::nullopt;
        if (!sourceId || !derivedId || !workspaceId || !source ||
            source->Task.Version != request.ExpectedTaskVersion || !root ||
            !std::filesystem::path{ *root }.is_absolute())
        {
            return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Derived Task context is invalid");
        }
        if (_active(*source))
        {
            return _failure(RuntimeTaskProjectionStatus::Conflict, "Active Task work prevents deriving a new execution context");
        }
        const auto contextId = NewRuntimeUuid();
        const auto terminalId = NewRuntimeUuid();
        if (!contextId || !terminalId)
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Derived Task identity generation failed");
        const auto now = (std::max)(RuntimeUnixMilliseconds(), source->Task.UpdatedAtUnixMilliseconds);
        const auto& draft = *request.Draft;
        CreateTaskCommand command;
        command.Task.Id = *derivedId;
        command.Task.Workspace = *workspaceId;
        command.Task.ForkedFromTaskId = *sourceId;
        command.Task.Title = draft.Title;
        command.Task.Description = draft.Description;
        for (size_t index = 0; index < draft.Acceptance.size(); ++index)
            command.Task.AcceptanceCriteria.push_back({ static_cast<uint32_t>(index), draft.Acceptance[index], false });
        command.Task.Status = TaskStatus::Todo;
        command.Task.Priority = static_cast<TaskPriority>(draft.Priority);
        command.Task.Version = 1;
        command.Task.CreatedAtUnixMilliseconds = now;
        command.Task.UpdatedAtUnixMilliseconds = now;
        command.ExecutionContext.Id = ExecutionContextId{ *contextId };
        command.ExecutionContext.Task = *derivedId;
        command.ExecutionContext.Mode = static_cast<ExecutionMode>(draft.ExecutionMode);
        command.ExecutionContext.SourceRoot = draft.SourceRoot;
        command.ExecutionContext.RelativeWorkingDirectory = draft.RelativeWorkingDirectory;
        command.ExecutionContext.Provider = draft.Provider;
        command.ExecutionContext.Model = draft.Model;
        command.ExecutionContext.Instructions = draft.Instructions;
        command.ExecutionContext.CreatedAtUnixMilliseconds = now;
        if (command.ExecutionContext.Mode == ExecutionMode::Direct)
            command.ExecutionContext.RuntimeRoot = draft.SourceRoot;
        else
        {
            const auto path = _runtimeDataRoot / L"task-worktrees" / L"v1" / _wide(derivedId->Value()).value();
            command.ExecutionContext.RuntimeRoot = _utf8(path.native()).value_or(std::string{});
            command.ExecutionContext.WorktreePath = command.ExecutionContext.RuntimeRoot;
            command.ExecutionContext.RepositoryCommonDirectory = _utf8((std::filesystem::path{ *root } / L".git").native()).value_or(std::string{});
            command.ExecutionContext.Branch = "refs/heads/wt-task/" + derivedId->Value();
        }
        command.Terminal.Id = TaskTerminalId{ *terminalId };
        command.Terminal.Task = *derivedId;
        command.Terminal.State = TaskTerminalState::NotCreated;
        command.Terminal.UpdatedAtUnixMilliseconds = now;
        TaskUiCommandResult anticipated{ request.CommandId, derivedId->Value(), std::nullopt, 1, 1 };
        const auto receipt = _receipt(anticipated);
        const auto metadata = receipt ? _metadata(request, 0, now, *receipt) : std::nullopt;
        if (!metadata)
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Derived Task metadata could not be created");
        command.Metadata = *metadata;
        command.Metadata.ExpectedTaskVersion = 0;
        return _stored(_store->CreateTask(command), std::move(anticipated));
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_restartProvider(
        const TaskUiCommandRequest& request,
        const HANDLE cancellationEvent)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        const auto aggregate = taskId ? _store->LoadTask(*taskId) : std::nullopt;
        if (!taskId || !aggregate || !_provider)
            return _failure(RuntimeTaskProjectionStatus::NotFound, "Task or provider was not found");
        const auto restarted = _provider->RestartProvider(cancellationEvent);
        if (!restarted.Succeeded())
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, restarted.Error);
        RuntimeTaskProjectionCommandResult result;
        result.Status = RuntimeTaskProjectionStatus::Succeeded;
        result.Result = { request.CommandId, request.TaskId, std::nullopt, aggregate->Task.Version, aggregate->LastEventSequence };
        result.Result.AffectedTaskCount = static_cast<uint32_t>((std::min)(restarted.AffectedTaskIds.size(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
        return result;
    }
}
