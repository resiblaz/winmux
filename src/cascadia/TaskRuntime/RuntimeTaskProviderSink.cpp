// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProviderSink.h"

#include "RuntimeCrypto.h"

#include <json/json.h>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskProvider;

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

        TaskProviderResult _failure(TaskProviderStatus status, std::string error)
        {
            return { status, std::move(error) };
        }

        TaskProviderStatus _providerStatus(const TaskStoreCommandStatus status) noexcept
        {
            switch (status)
            {
            case TaskStoreCommandStatus::Succeeded:
            case TaskStoreCommandStatus::Replayed:
                return TaskProviderStatus::Succeeded;
            case TaskStoreCommandStatus::NotFound:
            case TaskStoreCommandStatus::Tombstoned:
                return TaskProviderStatus::NotFound;
            case TaskStoreCommandStatus::VersionConflict:
            case TaskStoreCommandStatus::ActiveRunExists:
            case TaskStoreCommandStatus::ActiveIntentExists:
                return TaskProviderStatus::Conflict;
            case TaskStoreCommandStatus::InvalidCommand:
            case TaskStoreCommandStatus::InvalidRecord:
            case TaskStoreCommandStatus::InvalidTransition:
            case TaskStoreCommandStatus::IdempotencyConflict:
                return TaskProviderStatus::InvalidRequest;
            case TaskStoreCommandStatus::CapacityExceeded:
                return TaskProviderStatus::OutputLimitExceeded;
            default:
                return TaskProviderStatus::ResourceFailure;
            }
        }

        TaskProviderResult _providerResult(const TaskStoreCommandResult& result)
        {
            return result.Succeeded() ?
                       TaskProviderResult{ TaskProviderStatus::Succeeded, {} } :
                       _failure(_providerStatus(result.Status), result.Error);
        }

        std::string _json(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            builder["indentation"] = "";
            return Json::writeString(builder, value);
        }

        std::optional<std::string> _digest(
            const std::string_view domain,
            std::initializer_list<std::string_view> values)
        {
            std::vector<uint8_t> bytes;
            const auto append = [&](const std::string_view value) {
                bytes.insert(bytes.end(), value.begin(), value.end());
                bytes.push_back(0);
            };
            append(domain);
            for (const auto value : values) append(value);
            return RuntimeSha256Hex(bytes);
        }

        bool _metadata(
            TaskCommandMetadata& metadata,
            const TaskAggregateSnapshot& aggregate,
            const std::string_view domain,
            const std::string_view identity,
            std::string payload,
            std::string& error)
        {
            const auto command = NewRuntimeUuid();
            const auto event = NewRuntimeUuid();
            const auto digest = _digest(domain, { aggregate.Task.Id.Value(), identity, payload });
            if (!command || !event || !digest || payload.size() > MaximumTaskStoreJsonBytes)
            {
                error = "Runtime provider command metadata could not be created";
                return false;
            }
            metadata.Command = CommandId{ *command };
            metadata.Event = TaskEventId{ *event };
            metadata.ExpectedTaskVersion = aggregate.Task.Version;
            metadata.RequestDigest = *digest;
            metadata.EventPayloadJson = std::move(payload);
            metadata.ReceiptResultJson = "{}";
            metadata.TimestampUnixMilliseconds = (std::max)(
                _unixMilliseconds(), aggregate.Task.UpdatedAtUnixMilliseconds);
            return true;
        }

        AgentSessionState _sessionState(const TaskProviderSessionState state) noexcept
        {
            switch (state)
            {
            case TaskProviderSessionState::Suspended: return AgentSessionState::Suspended;
            case TaskProviderSessionState::Poisoned: return AgentSessionState::Poisoned;
            case TaskProviderSessionState::Closed: return AgentSessionState::Closed;
            default: return AgentSessionState::Available;
            }
        }

        TaskRunState _runState(const TaskProviderRunState state) noexcept
        {
            switch (state)
            {
            case TaskProviderRunState::Starting: return TaskRunState::Starting;
            case TaskProviderRunState::Running: return TaskRunState::Running;
            case TaskProviderRunState::Completed: return TaskRunState::Completed;
            case TaskProviderRunState::Failed: return TaskRunState::Failed;
            case TaskProviderRunState::Interrupted: return TaskRunState::Interrupted;
            default: return TaskRunState::Failed;
            }
        }

        std::optional<TaskRunState> _nextRunState(
            const TaskRunState current,
            const TaskRunState target) noexcept
        {
            if (current == target) return std::nullopt;
            if (target == TaskRunState::Running)
            {
                if (current == TaskRunState::Queued) return TaskRunState::Preparing;
                if (current == TaskRunState::Preparing || current == TaskRunState::WaitingForDirectory) return TaskRunState::Starting;
                if (current == TaskRunState::Starting) return TaskRunState::Running;
            }
            return target;
        }

        std::string _eventPayload(const TaskProviderEvent& event)
        {
            Json::Value value{ Json::objectValue };
            value["providerSequence"] = Json::UInt64{ event.Sequence };
            value["kind"] = std::string{ TaskProviderEventKindName(event.Kind) };
            value["providerSessionId"] = event.SessionId;
            value["providerRunId"] = event.RunId;
            if (!event.ItemId.empty()) value["itemId"] = event.ItemId;
            if (!event.ApprovalId.empty()) value["approvalId"] = event.ApprovalId;
            if (!event.Text.empty())
            {
                value["text"] = event.Text.substr(0, 32 * 1024);
                if (event.Text.size() > 32 * 1024) value["truncated"] = true;
            }
            value["redacted"] = event.Redacted;
            if (!value.isMember("truncated")) value["truncated"] = event.Truncated;
            value["originalBytes"] = Json::UInt64{ event.OriginalBytes };
            if (event.PayloadJson.size() <= 48 * 1024)
            {
                Json::CharReaderBuilder builder;
                Json::Value providerPayload;
                std::string errors;
                const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
                if (reader && reader->parse(
                                  event.PayloadJson.data(),
                                  event.PayloadJson.data() + event.PayloadJson.size(),
                                  &providerPayload, &errors))
                {
                    value["providerPayload"] = std::move(providerPayload);
                }
            }
            auto encoded = _json(value);
            if (encoded.size() > MaximumTaskStoreJsonBytes)
            {
                value.removeMember("providerPayload");
                value["truncated"] = true;
                encoded = _json(value);
            }
            return encoded;
        }
    }

    RuntimeTaskProviderSink::RuntimeTaskProviderSink(
        std::shared_ptr<ITaskStore> store,
        std::shared_ptr<RuntimeTaskTerminalService> terminals) noexcept :
        _store{ std::move(store) },
        _terminals{ std::move(terminals) }
    {
    }

    bool RuntimeTaskProviderSink::Valid() const noexcept
    {
        return _store && _store->IsReady();
    }

    TaskProviderResult RuntimeTaskProviderSink::OnSessionStateChanged(
        const TaskProviderSessionBinding& session)
    {
        const std::scoped_lock lock{ _mutex };
        const auto taskId = ParseId<TaskId>(session.TaskId);
        if (!Valid() || !taskId) return _failure(TaskProviderStatus::InvalidRequest, "Runtime provider session Task is invalid");
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto aggregate = _store->LoadTask(*taskId);
            if (!aggregate) return _failure(TaskProviderStatus::NotFound, "Runtime provider session Task was not found");
            const auto existing = std::find_if(
                aggregate->AgentSessions.begin(), aggregate->AgentSessions.end(),
                [&](const auto& candidate) {
                    return candidate.Provider == "codex" &&
                           candidate.ProviderSessionId == session.ProviderSessionId;
                });
            const auto requestedState = _sessionState(session.State);
            const auto requestedCurrent = requestedState != AgentSessionState::Closed &&
                                          requestedState != AgentSessionState::Poisoned;
            if (existing != aggregate->AgentSessions.end() &&
                existing->State == requestedState && existing->IsCurrent == requestedCurrent)
            {
                return { TaskProviderStatus::AlreadyApplied, {} };
            }
            const auto now = (std::max)(_unixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
            RecordTaskAgentSessionCommand command;
            command.Session.Id = existing != aggregate->AgentSessions.end() ? existing->Id : AgentSessionId{};
            if (!command.Session.Id)
            {
                const auto id = NewRuntimeUuid();
                if (!id) return _failure(TaskProviderStatus::ResourceFailure, "Runtime AgentSession identity generation failed");
                command.Session.Id = AgentSessionId{ *id };
            }
            command.Session.Task = *taskId;
            command.Session.Provider = "codex";
            command.Session.ProviderSessionId = session.ProviderSessionId;
            command.Session.State = requestedState;
            command.Session.IsCurrent = requestedCurrent;
            command.Session.CreatedAtUnixMilliseconds = existing != aggregate->AgentSessions.end() ?
                                                             existing->CreatedAtUnixMilliseconds : now;
            command.Session.UpdatedAtUnixMilliseconds = now;
            Json::Value payload{ Json::objectValue };
            payload["provider"] = "codex";
            payload["providerSessionId"] = session.ProviderSessionId;
            payload["state"] = static_cast<uint32_t>(requestedState);
            payload["current"] = requestedCurrent;
            std::string error;
            if (!_metadata(command.Metadata, *aggregate, "provider-session-v1", session.ProviderSessionId, _json(payload), error))
            {
                return _failure(TaskProviderStatus::ResourceFailure, std::move(error));
            }
            const auto stored = _store->RecordTaskAgentSession(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict) continue;
            return _providerResult(stored);
        }
        return _failure(TaskProviderStatus::Conflict, "Runtime provider session changed concurrently");
    }

    TaskProviderResult RuntimeTaskProviderSink::OnRunStateChanged(
        const TaskProviderRunBinding& binding,
        const std::optional<TaskProviderFailureDisposition>& failure)
    {
        const std::scoped_lock lock{ _mutex };
        const auto taskId = ParseId<TaskId>(binding.TaskId);
        const auto runId = ParseId<TaskRunId>(binding.TaskRunId);
        if (!Valid() || !taskId || !runId) return _failure(TaskProviderStatus::InvalidRequest, "Runtime provider Run identity is invalid");
        const auto target = _runState(binding.State);
        for (size_t transition = 0; transition < 4; ++transition)
        {
            const auto aggregate = _store->LoadTask(*taskId);
            if (!aggregate) return _failure(TaskProviderStatus::NotFound, "Runtime provider Run Task was not found");
            const auto run = std::find_if(aggregate->Runs.begin(), aggregate->Runs.end(), [&](const auto& candidate) { return candidate.Id == *runId; });
            if (run == aggregate->Runs.end()) return _failure(TaskProviderStatus::NotFound, "Runtime provider TaskRun was not found");
            if (run->State == target) return { TaskProviderStatus::AlreadyApplied, {} };
            const auto next = _nextRunState(run->State, target);
            if (!next) return { TaskProviderStatus::AlreadyApplied, {} };

            ChangeTaskRunStateCommand command;
            command.Task = *taskId;
            command.Run = *runId;
            command.RequestedState = *next;
            if (failure && (*next == TaskRunState::Failed || *next == TaskRunState::Interrupted))
            {
                command.ReasonCode = failure->ReasonCode;
            }
            Json::Value payload{ Json::objectValue };
            payload["provider"] = "codex";
            payload["providerSessionId"] = binding.ProviderSessionId;
            payload["providerRunId"] = binding.ProviderRunId;
            payload["state"] = static_cast<uint32_t>(*next);
            if (failure)
            {
                payload["failureClass"] = static_cast<uint32_t>(failure->Classification);
                payload["resumeSafety"] = static_cast<uint32_t>(failure->ResumeSafety);
                payload["automaticRetryAllowed"] = failure->AutomaticRetryAllowed;
                payload["maximumExecutions"] = failure->MaximumExecutions;
                payload["reasonCode"] = failure->ReasonCode;
            }
            std::string error;
            if (!_metadata(command.Metadata, *aggregate, "provider-run-state-v1", binding.TaskRunId, _json(payload), error))
            {
                return _failure(TaskProviderStatus::ResourceFailure, std::move(error));
            }
            const auto stored = _store->ChangeTaskRunState(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict) continue;
            if (!stored.Succeeded()) return _providerResult(stored);
            if (*next == target) return { TaskProviderStatus::Succeeded, {} };
        }
        return _failure(TaskProviderStatus::Conflict, "Runtime provider Run could not reach its target state");
    }

    TaskProviderResult RuntimeTaskProviderSink::OnEvent(
        const TaskProviderSessionBinding& session,
        const std::optional<TaskProviderRunBinding>& run,
        const TaskProviderEvent& event)
    {
        const std::scoped_lock lock{ _mutex };
        const auto taskId = ParseId<TaskId>(session.TaskId);
        const auto runId = run ? ParseId<TaskRunId>(run->TaskRunId) : std::optional<TaskRunId>{};
        if (!Valid() || !taskId || (run && !runId)) return _failure(TaskProviderStatus::InvalidRequest, "Runtime provider event identity is invalid");
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto aggregate = _store->LoadTask(*taskId);
            if (!aggregate) return _failure(TaskProviderStatus::NotFound, "Runtime provider event Task was not found");
            AppendTaskProviderEventCommand command;
            command.Task = *taskId;
            command.Run = runId;
            command.ProviderSequence = event.Sequence;
            command.EventType = "Provider_" + std::string{ TaskProviderEventKindName(event.Kind) };
            const auto payload = _eventPayload(event);
            std::string error;
            if (!_metadata(command.Metadata, *aggregate, "provider-event-v1", std::to_string(event.Sequence), payload, error))
            {
                return _failure(TaskProviderStatus::ResourceFailure, std::move(error));
            }
            const auto stored = _store->AppendTaskProviderEvent(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict) continue;
            if (stored.Succeeded())
            {
                if (const auto terminals = _terminals.lock())
                {
                    static_cast<void>(terminals->PublishEvent(session.TaskId, event));
                }
            }
            return _providerResult(stored);
        }
        return _failure(TaskProviderStatus::Conflict, "Runtime provider event changed concurrently");
    }

    TaskProviderResult RuntimeTaskProviderSink::OnSteeringDeferred(
        const TaskProviderRunBinding& binding,
        const std::string_view message,
        const std::string_view reasonCode)
    {
        const std::scoped_lock lock{ _mutex };
        const auto taskId = ParseId<TaskId>(binding.TaskId);
        if (!Valid() || !taskId || message.empty() || message.size() > MaximumTaskMessageBytes)
        {
            return _failure(TaskProviderStatus::InvalidRequest, "Runtime deferred steering message is invalid");
        }
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto aggregate = _store->LoadTask(*taskId);
            if (!aggregate) return _failure(TaskProviderStatus::NotFound, "Runtime deferred steering Task was not found");
            const auto id = NewRuntimeUuid();
            if (!id) return _failure(TaskProviderStatus::ResourceFailure, "Runtime deferred message identity generation failed");
            const auto now = (std::max)(_unixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
            EnqueueTaskMessageCommand command;
            command.Message.Id = TaskMessageId{ *id };
            command.Message.Task = *taskId;
            command.Message.Author = TaskMessageAuthor::User;
            command.Message.State = TaskMessageState::Pending;
            command.Message.Revision = 1;
            command.Message.Content = std::string{ message };
            command.Message.CreatedAtUnixMilliseconds = now;
            command.Message.UpdatedAtUnixMilliseconds = now;
            Json::Value payload{ Json::objectValue };
            payload["reasonCode"] = std::string{ reasonCode };
            payload["providerRunId"] = binding.ProviderRunId;
            std::string error;
            if (!_metadata(command.Metadata, *aggregate, "provider-steering-deferred-v1", binding.TaskRunId, _json(payload), error))
            {
                return _failure(TaskProviderStatus::ResourceFailure, std::move(error));
            }
            const auto stored = _store->EnqueueTaskMessage(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict) continue;
            return _providerResult(stored);
        }
        return _failure(TaskProviderStatus::Conflict, "Runtime deferred steering changed concurrently");
    }
}
