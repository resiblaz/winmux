// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProjectionService.h"

#include "RuntimeCrypto.h"
#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        RuntimeTaskProjectionCommandResult _failure(
            const RuntimeTaskProjectionStatus status,
            std::string error)
        {
            RuntimeTaskProjectionCommandResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }

        RuntimeTaskProjectionStatus _storeStatus(const TaskStoreCommandStatus status) noexcept
        {
            if (status == TaskStoreCommandStatus::NotFound || status == TaskStoreCommandStatus::Tombstoned)
                return RuntimeTaskProjectionStatus::NotFound;
            if (status == TaskStoreCommandStatus::VersionConflict || status == TaskStoreCommandStatus::InvalidTransition)
                return RuntimeTaskProjectionStatus::Conflict;
            if (status == TaskStoreCommandStatus::NotReady)
                return RuntimeTaskProjectionStatus::StoreUnavailable;
            return RuntimeTaskProjectionStatus::ResourceFailure;
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

        std::optional<std::string> _receipt(const TaskUiCommandResult& result)
        {
            const auto encoded = EncodeTaskUiCommandResult(result);
            return encoded.Succeeded ? std::optional<std::string>{ encoded.Payload } : std::nullopt;
        }

        bool _metadata(
            TaskCommandMetadata& metadata,
            const TaskUiCommandRequest& request,
            const uint64_t expectedVersion,
            const int64_t timestamp,
            const std::string& receipt)
        {
            const auto command = ParseId<CommandId>(request.CommandId);
            const auto event = NewRuntimeUuid();
            const auto encoded = EncodeTaskUiCommandRequest(request);
            const auto digest = encoded.Succeeded ? RuntimeSha256Hex(std::span<const uint8_t>{
                                                        reinterpret_cast<const uint8_t*>(encoded.Payload.data()), encoded.Payload.size() }) :
                                                    std::nullopt;
            if (!command || !event || !digest || receipt.empty())
                return false;
            metadata.Command = *command;
            metadata.Event = TaskEventId{ *event };
            metadata.ExpectedTaskVersion = expectedVersion;
            metadata.RequestDigest = *digest;
            metadata.EventPayloadJson = R"({"source":"terminal_tasks_page"})";
            metadata.ReceiptResultJson = receipt;
            metadata.TimestampUnixMilliseconds = timestamp;
            return true;
        }
    }

    RuntimeTaskProjectionCommandResult RuntimeTaskProjectionService::_open(
        const TaskUiCommandRequest& request,
        const std::string_view clientInstanceId)
    {
        const auto taskId = ParseId<TaskId>(request.TaskId);
        if (!taskId || !_terminals || !IsCanonicalProtocolUuid(clientInstanceId))
        {
            return _failure(RuntimeTaskProjectionStatus::InvalidRequest, "Task terminal open context is invalid");
        }
        auto aggregate = _store->LoadTask(*taskId);
        if (!aggregate)
            return _failure(RuntimeTaskProjectionStatus::NotFound, "Task was not found");

        auto generation = aggregate->Terminal.Generation;
        const auto startsGeneration = aggregate->Terminal.State == TaskTerminalState::NotCreated ||
                                      aggregate->Terminal.State == TaskTerminalState::Exited ||
                                      aggregate->Terminal.State == TaskTerminalState::Suspended ||
                                      aggregate->Terminal.State == TaskTerminalState::Error ||
                                      aggregate->Terminal.State == TaskTerminalState::Closed;
        const auto mutatesTerminal = startsGeneration || aggregate->Task.ManualHold;
        if (startsGeneration)
        {
            if (generation == (std::numeric_limits<uint64_t>::max)())
            {
                return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task terminal generation is exhausted");
            }
            ++generation;
        }
        else if (aggregate->Terminal.State != TaskTerminalState::Starting &&
                 aggregate->Terminal.State != TaskTerminalState::Ready &&
                 aggregate->Terminal.State != TaskTerminalState::Busy)
        {
            return _failure(RuntimeTaskProjectionStatus::Conflict, "Task terminal cannot be opened from its current state");
        }

        const auto endpoint = _terminals->Endpoint(aggregate->Terminal.Id.Value(), generation);
        const auto pipeName = endpoint.Succeeded() ? _utf8(endpoint.Endpoint.PipeName) : std::nullopt;
        if (!endpoint.Succeeded() || !pipeName)
        {
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task terminal endpoint could not be created");
        }
        TaskUiCommandResult anticipated;
        anticipated.CommandId = request.CommandId;
        anticipated.TaskId = request.TaskId;
        anticipated.TaskVersion = aggregate->Task.Version + static_cast<uint64_t>(mutatesTerminal);
        anticipated.EventSequence = aggregate->LastEventSequence + static_cast<uint64_t>(mutatesTerminal);
        anticipated.Surface = TaskUiSurfaceDescriptor{
            aggregate->Terminal.Id.Value(), *pipeName, generation, false
        };

        bool replayed{};
        if (mutatesTerminal)
        {
            ChangeTaskTerminalStateCommand command;
            command.Task = *taskId;
            command.RequestedState = startsGeneration ? TaskTerminalState::Starting : aggregate->Terminal.State;
            command.RequestedGeneration = generation;
            command.ExplicitOpen = startsGeneration;
            command.ClearManualHold = true;
            const auto receipt = _receipt(anticipated);
            if (!receipt || !_metadata(
                                command.Metadata, request, request.ExpectedTaskVersion, aggregate->Task.UpdatedAtUnixMilliseconds, *receipt))
            {
                return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task terminal open metadata could not be created");
            }
            const auto stored = _store->ChangeTaskTerminalState(command);
            if (!stored.Succeeded())
                return _failure(_storeStatus(stored.Status), stored.Error);
            replayed = stored.Status == TaskStoreCommandStatus::Replayed;
            aggregate = _store->LoadTask(*taskId);
            if (!aggregate)
                return _failure(RuntimeTaskProjectionStatus::NotFound, "Task disappeared while opening its terminal");
        }

        const auto opened = _terminals->Open({ request.TaskId, aggregate->Terminal.Id.Value(), generation, std::string{ clientInstanceId }, aggregate->Task.Title });
        if (opened.Status == RuntimeTaskTerminalServiceStatus::OwnedElsewhere)
        {
            anticipated.Surface->OwnedElsewhere = true;
        }
        else if (!opened.Succeeded())
        {
            return _failure(RuntimeTaskProjectionStatus::ResourceFailure, opened.Error);
        }

        if (aggregate->Terminal.State == TaskTerminalState::Starting)
        {
            const auto commandId = NewRuntimeUuid();
            if (!commandId)
                return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task terminal ready identity could not be created");
            auto internal = request;
            internal.CommandId = *commandId;
            internal.ExpectedTaskVersion = aggregate->Task.Version;
            ChangeTaskTerminalStateCommand ready;
            ready.Task = *taskId;
            ready.RequestedState = TaskTerminalState::Ready;
            ready.RequestedGeneration = generation;
            anticipated.TaskVersion = aggregate->Task.Version + 1;
            anticipated.EventSequence = aggregate->LastEventSequence + 1;
            const auto receipt = _receipt(anticipated);
            if (!receipt || !_metadata(
                                ready.Metadata, internal, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds, *receipt))
            {
                return _failure(RuntimeTaskProjectionStatus::ResourceFailure, "Task terminal ready metadata could not be created");
            }
            const auto stored = _store->ChangeTaskTerminalState(ready);
            if (!stored.Succeeded())
            {
                static_cast<void>(_terminals->Close(request.TaskId, generation, false));
                return _failure(_storeStatus(stored.Status), stored.Error);
            }
            anticipated.TaskVersion = stored.CommittedTaskVersion;
            anticipated.EventSequence = stored.LastEventSequence;
        }
        else
        {
            anticipated.TaskVersion = aggregate->Task.Version;
            anticipated.EventSequence = aggregate->LastEventSequence;
        }
        anticipated.Replayed = replayed;
        return { replayed ? RuntimeTaskProjectionStatus::Replayed : RuntimeTaskProjectionStatus::Succeeded, anticipated, {} };
    }
}
