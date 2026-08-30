// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeRtaskCommandService.h"

#include "RuntimeCrypto.h"

#include "ProtocolValidation.h"
#include "TaskRunStatePolicy.h"

#include <json/json.h>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        TaskAgentMutationKind _mutationKind(const RtaskMethod method) noexcept
        {
            switch (method)
            {
            case RtaskMethod::StatusInProgress:
                return TaskAgentMutationKind::StatusInProgress;
            case RtaskMethod::StatusInReview:
                return TaskAgentMutationKind::StatusInReview;
            case RtaskMethod::StatusBlocked:
                return TaskAgentMutationKind::StatusBlocked;
            case RtaskMethod::Result:
                return TaskAgentMutationKind::Result;
            default:
                return TaskAgentMutationKind::Count;
            }
        }

        std::optional<std::string> _requestDigest(const RtaskCommandRequest& request)
        {
            std::vector<uint8_t> input;
            const auto method = RtaskMethodName(request.Method);
            input.reserve(request.RunId.size() + method.size() + request.Text.size() + 3);
            input.insert(input.end(), request.RunId.begin(), request.RunId.end());
            input.emplace_back(uint8_t{ 0 });
            input.insert(input.end(), method.begin(), method.end());
            input.emplace_back(uint8_t{ 0 });
            input.insert(input.end(), request.Text.begin(), request.Text.end());
            input.emplace_back(uint8_t{ 0 });
            return RuntimeSha256Hex(input);
        }

        std::string _writeJson(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            builder["indentation"] = "";
            return Json::writeString(builder, value);
        }

        std::string _eventPayload(const RtaskCommandRequest& request)
        {
            Json::Value payload{ Json::objectValue };
            payload["method"] = std::string{ RtaskMethodName(request.Method) };
            if (request.Method == RtaskMethod::StatusBlocked)
            {
                payload["reason"] = request.Text;
            }
            else if (request.Method == RtaskMethod::Result)
            {
                payload["textBytes"] = Json::UInt64{ request.Text.size() };
            }
            return _writeJson(payload);
        }

        RuntimeRtaskCommandStatus _serviceStatus(
            const TaskStoreCommandStatus status) noexcept
        {
            switch (status)
            {
            case TaskStoreCommandStatus::Succeeded:
                return RuntimeRtaskCommandStatus::Succeeded;
            case TaskStoreCommandStatus::Replayed:
                return RuntimeRtaskCommandStatus::Replayed;
            case TaskStoreCommandStatus::NotFound:
            case TaskStoreCommandStatus::Tombstoned:
                return RuntimeRtaskCommandStatus::NotFound;
            case TaskStoreCommandStatus::VersionConflict:
            case TaskStoreCommandStatus::IdempotencyConflict:
            case TaskStoreCommandStatus::InvalidTransition:
            case TaskStoreCommandStatus::ActiveRunExists:
            case TaskStoreCommandStatus::ActiveIntentExists:
                return RuntimeRtaskCommandStatus::Conflict;
            case TaskStoreCommandStatus::NotReady:
                return RuntimeRtaskCommandStatus::StoreUnavailable;
            case TaskStoreCommandStatus::InvalidCommand:
            case TaskStoreCommandStatus::InvalidRecord:
            case TaskStoreCommandStatus::CapacityExceeded:
                return RuntimeRtaskCommandStatus::InvalidRequest;
            case TaskStoreCommandStatus::StorageError:
            default:
                return RuntimeRtaskCommandStatus::ResourceFailure;
            }
        }

        bool _activeAgentRun(const TaskRunRecord& run) noexcept
        {
            return run.State == TaskRunState::Starting ||
                   run.State == TaskRunState::Running;
        }
    }

    RuntimeRtaskCommandService::RuntimeRtaskCommandService(
        std::shared_ptr<ITaskStore> store,
        std::shared_ptr<RuntimeRtaskCapabilityRegistry> capabilities) noexcept :
        _store{ std::move(store) },
        _capabilities{ std::move(capabilities) }
    {
        _valid = _store && _store->IsReady() && _capabilities &&
                 _capabilities->Valid();
    }

    bool RuntimeRtaskCommandService::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeRtaskCapabilityResult RuntimeRtaskCommandService::IssueForActiveRun(
        const std::string_view taskId,
        const std::string_view runId,
        const RtaskMethodSet methods,
        const uint32_t lifetimeMilliseconds,
        const RuntimeRtaskMilliseconds now)
    {
        std::scoped_lock lock{ _mutex };
        RuntimeRtaskCapabilityResult result;
        if (!_valid)
        {
            return result;
        }
        const auto task = ParseId<TaskId>(taskId);
        const auto run = ParseId<TaskRunId>(runId);
        if (!task)
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidTask;
            return result;
        }
        if (!run)
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidRun;
            return result;
        }
        const auto snapshot = _store->LoadTask(*task);
        if (!snapshot)
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidTask;
            return result;
        }
        const auto found = std::ranges::find_if(snapshot->Runs, [&](const auto& candidate) {
            return candidate.Id == *run;
        });
        if (found == snapshot->Runs.end() || !_activeAgentRun(*found))
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidRun;
            return result;
        }
        return _capabilities->IssueOrRenew(
            taskId,
            runId,
            methods,
            lifetimeMilliseconds,
            now);
    }

    RuntimeRtaskCapabilityResult RuntimeRtaskCommandService::InvalidateRun(
        const std::string_view runId) noexcept
    {
        std::scoped_lock lock{ _mutex };
        return _capabilities ?
                   _capabilities->InvalidateRun(runId) :
                   RuntimeRtaskCapabilityResult{};
    }

    RuntimeRtaskCommandResult RuntimeRtaskCommandService::Execute(
        const RtaskCommandRequest& request,
        const RuntimeRtaskCommandContext& context)
    {
        std::scoped_lock lock{ _mutex };
        RuntimeRtaskCommandResult result;
        if (!_valid)
        {
            return result;
        }
        std::string validationError;
        if (!ValidateRtaskCommand(request, validationError) ||
            context.UnixMilliseconds < 0 ||
            !IsCanonicalProtocolUuid(context.EventId))
        {
            result.Status = RuntimeRtaskCommandStatus::InvalidRequest;
            result.Error = validationError.empty() ?
                               "rtask command context is invalid" :
                               std::move(validationError);
            return result;
        }

        const auto authorization = _capabilities->Authorize(
            request.Capability,
            request.RunId,
            request.Method,
            context.MonotonicMilliseconds);
        result.CapabilityStatus = authorization.Status;
        if (authorization.Status != RuntimeRtaskCapabilityStatus::Authorized ||
            !authorization.Binding)
        {
            result.Status = authorization.Status ==
                                        RuntimeRtaskCapabilityStatus::ResourceFailure ?
                                RuntimeRtaskCommandStatus::ResourceFailure :
                                RuntimeRtaskCommandStatus::Unauthorized;
            result.Error = "rtask capability authorization failed";
            return result;
        }

        const auto taskId = ParseId<TaskId>(authorization.Binding->TaskId);
        const auto runId = ParseId<TaskRunId>(authorization.Binding->RunId);
        const auto commandId = ParseId<CommandId>(request.CommandId);
        const auto eventId = ParseId<TaskEventId>(context.EventId);
        const auto digest = _requestDigest(request);
        const auto kind = _mutationKind(request.Method);
        if (!taskId || !runId || !commandId || !eventId || !digest ||
            kind == TaskAgentMutationKind::Count)
        {
            result.Status = RuntimeRtaskCommandStatus::ResourceFailure;
            result.Error = "rtask command metadata construction failed";
            return result;
        }

        TaskStoreCommandResult stored;
        for (size_t attempt = 0; attempt < 2; ++attempt)
        {
            const auto snapshot = _store->LoadTask(*taskId);
            if (!snapshot)
            {
                result.Status = RuntimeRtaskCommandStatus::NotFound;
                result.Error = "Capability-bound Task was not found";
                return result;
            }

            ApplyTaskAgentMutationCommand command;
            command.Metadata.Command = *commandId;
            command.Metadata.Event = *eventId;
            command.Metadata.ExpectedTaskVersion = snapshot->Task.Version;
            command.Metadata.RequestDigest = *digest;
            command.Metadata.EventPayloadJson = _eventPayload(request);
            command.Metadata.ReceiptResultJson = "{}";
            command.Metadata.TimestampUnixMilliseconds = (std::max)(
                context.UnixMilliseconds,
                snapshot->Task.UpdatedAtUnixMilliseconds);
            command.Task = *taskId;
            command.Run = *runId;
            command.Kind = kind;
            command.Text = request.Text;
            stored = _store->ApplyTaskAgentMutation(command);
            if (stored.Status != TaskStoreCommandStatus::VersionConflict)
            {
                break;
            }
        }

        result.StoreStatus = stored.Status;
        result.Status = _serviceStatus(stored.Status);
        result.Error = std::move(stored.Error);
        if (!stored.Succeeded())
        {
            return result;
        }
        result.Result = {
            request.CommandId,
            stored.Status == TaskStoreCommandStatus::Replayed ?
                RtaskCommandDisposition::Replayed :
                RtaskCommandDisposition::Applied,
            stored.CommittedTaskVersion,
            stored.LastEventSequence,
        };
        return result;
    }
}
