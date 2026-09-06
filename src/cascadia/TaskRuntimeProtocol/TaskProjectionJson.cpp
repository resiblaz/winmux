// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProjectionRpc.h"

#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

#include <json/json.h>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        template<typename TValue>
        ProtocolRpcJsonDecodeResult<TValue> _failure(std::string error)
        {
            ProtocolRpcJsonDecodeResult<TValue> result;
            result.Status = ProtocolRpcJsonStatus::InvalidSchema;
            result.Error = std::move(error);
            return result;
        }

        bool _exact(const Json::Value& value, const std::initializer_list<std::string_view> names)
        {
            return value.isObject() && value.size() == names.size() &&
                   std::ranges::all_of(names, [&](const auto name) { return value.isMember(std::string{ name }); });
        }

        bool _unsigned(const Json::Value& value) noexcept
        {
            return value.isUInt64() || (value.isInt64() && value.asInt64() >= 0);
        }

        bool _optionalString(const Json::Value& value, std::optional<std::string>& result)
        {
            if (value.isNull())
            {
                result.reset();
                return true;
            }
            if (!value.isString()) return false;
            result = value.asString();
            return true;
        }

        Json::Value _optional(const std::optional<std::string>& value)
        {
            return value ? Json::Value{ *value } : Json::Value{};
        }

        Json::Value _optional(const std::optional<int64_t>& value)
        {
            return value ? Json::Value{ Json::Int64{ *value } } : Json::Value{};
        }

        Json::Value _optional(const std::optional<uint64_t>& value)
        {
            return value ? Json::Value{ Json::UInt64{ *value } } : Json::Value{};
        }

        template<typename TValue>
        bool _decodeRoot(const std::string_view payload, Json::Value& root, ProtocolRpcJsonDecodeResult<TValue>& failure)
        {
            if (payload.size() > MaximumControlFrameBytes || !IsValidUtf8(payload))
            {
                failure.Status = payload.size() > MaximumControlFrameBytes ? ProtocolRpcJsonStatus::Oversized : ProtocolRpcJsonStatus::InvalidUtf8;
                failure.Error = "Task projection JSON exceeds protocol bounds";
                return false;
            }
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false;
            builder["allowTrailingCommas"] = false;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            std::string error;
            if (!reader || !reader->parse(payload.data(), payload.data() + payload.size(), &root, &error) || !root.isObject())
            {
                failure.Status = ProtocolRpcJsonStatus::MalformedJson;
                failure.Error = "Task projection JSON is malformed";
                return false;
            }
            return true;
        }

        ProtocolRpcJsonEncodeResult _encode(Json::Value value)
        {
            Json::StreamWriterBuilder builder;
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            builder["indentation"] = "";
            ProtocolRpcJsonEncodeResult result;
            result.Payload = Json::writeString(builder, value);
            if (result.Payload.empty() || result.Payload.size() > MaximumControlFrameBytes || !IsValidUtf8(result.Payload))
            {
                result.Payload.clear();
                result.Error = "Encoded Task projection exceeds protocol bounds";
                return result;
            }
            result.Succeeded = true;
            return result;
        }

        Json::Value _agentSnapshot(const std::optional<AgentProfileReference>& agent, const std::string& name)
        {
            Json::Value result{ Json::objectValue };
            result["id"] = agent ? Json::Value{ agent->Id } : Json::Value{};
            result["revision"] = Json::UInt64{ agent ? agent->Revision : 0 };
            result["name"] = name;
            return result;
        }

        bool _readAgentSnapshot(const Json::Value& value, std::optional<AgentProfileReference>& agent, std::string& name)
        {
            if (!_exact(value, { "id", "revision", "name" }) ||
                (!value["id"].isNull() && !value["id"].isString()) || !_unsigned(value["revision"]) || !value["name"].isString()) return false;
            if (value["id"].isString()) agent = AgentProfileReference{ value["id"].asString(), value["revision"].asUInt64() };
            else if (value["revision"].asUInt64()) return false;
            name = value["name"].asString();
            return true;
        }

        Json::Value _execution(const TaskProjectionExecutionRecord& value, const bool includeAgents)
        {
            Json::Value result{ Json::objectValue };
            result["id"] = value.Id;
            result["revision"] = Json::UInt64{ value.Revision };
            result["mode"] = static_cast<uint32_t>(value.Mode);
            result["sourceRoot"] = value.SourceRoot;
            result["runtimeRoot"] = value.RuntimeRoot;
            result["worktreePath"] = value.WorktreePath;
            result["branch"] = value.Branch;
            result["relativeWorkingDirectory"] = value.RelativeWorkingDirectory;
            result["provider"] = value.Provider;
            result["model"] = value.Model;
            result["instructions"] = value.Instructions;
            if (includeAgents) result["agent"] = _agentSnapshot(value.Agent, value.AgentName);
            return result;
        }

        Json::Value _task(const TaskProjectionRecord& value, const bool includeAgents)
        {
            Json::Value result{ Json::objectValue };
            result["id"] = value.Id;
            result["workspaceId"] = value.WorkspaceId;
            result["title"] = value.Title;
            result["description"] = value.Description;
            result["acceptance"] = Json::Value{ Json::arrayValue };
            for (const auto& item : value.Acceptance)
            {
                Json::Value encoded{ Json::objectValue };
                encoded["text"] = item.Text;
                encoded["satisfied"] = item.Satisfied;
                result["acceptance"].append(std::move(encoded));
            }
            result["status"] = static_cast<uint32_t>(value.Status);
            result["priority"] = static_cast<uint32_t>(value.Priority);
            result["version"] = Json::UInt64{ value.Version };
            result["manualHold"] = value.ManualHold;
            result["createdAt"] = Json::Int64{ value.CreatedAtUnixMilliseconds };
            result["updatedAt"] = Json::Int64{ value.UpdatedAtUnixMilliseconds };
            result["execution"] = _execution(value.Execution, includeAgents);
            result["terminal"]["state"] = static_cast<uint32_t>(value.Terminal.State);
            result["terminal"]["generation"] = Json::UInt64{ value.Terminal.Generation };
            result["terminal"]["currentAgentSessionId"] = _optional(value.Terminal.CurrentAgentSessionId);
            result["agentSessions"] = Json::Value{ Json::arrayValue };
            for (const auto& session : value.AgentSessions)
            {
                Json::Value encoded{ Json::objectValue };
                encoded["id"] = session.Id;
                encoded["provider"] = session.Provider;
                encoded["providerSessionId"] = session.ProviderSessionId;
                encoded["state"] = session.State;
                encoded["isCurrent"] = session.IsCurrent;
                result["agentSessions"].append(std::move(encoded));
            }
            result["runs"] = Json::Value{ Json::arrayValue };
            for (const auto& run : value.Runs)
            {
                Json::Value encoded{ Json::objectValue };
                encoded["id"] = run.Id;
                encoded["executionContextId"] = run.ExecutionContextId;
                encoded["agentSessionId"] = _optional(run.AgentSessionId);
                encoded["retryOfRunId"] = _optional(run.RetryOfRunId);
                encoded["trigger"] = run.Trigger;
                encoded["state"] = static_cast<uint32_t>(run.State);
                encoded["sequence"] = Json::UInt64{ run.Sequence };
                encoded["createdAt"] = Json::Int64{ run.CreatedAtUnixMilliseconds };
                encoded["startedAt"] = _optional(run.StartedAtUnixMilliseconds);
                encoded["finishedAt"] = _optional(run.FinishedAtUnixMilliseconds);
                encoded["result"] = run.Result;
                encoded["reasonCode"] = run.ReasonCode;
                if (includeAgents)
                {
                    auto execution = _agentSnapshot(run.Agent, run.AgentName);
                    execution["provider"] = run.Provider;
                    execution["model"] = run.Model;
                    execution["contextRevision"] = Json::UInt64{ run.ExecutionRevision };
                    encoded["executionAgent"] = std::move(execution);
                }
                result["runs"].append(std::move(encoded));
            }
            result["messages"] = Json::Value{ Json::arrayValue };
            for (const auto& message : value.Messages)
            {
                Json::Value encoded{ Json::objectValue };
                encoded["id"] = message.Id;
                encoded["author"] = message.Author;
                encoded["state"] = message.State;
                encoded["revision"] = Json::UInt64{ message.Revision };
                encoded["content"] = message.Content;
                encoded["claimedByRunId"] = _optional(message.ClaimedByRunId);
                encoded["createdAt"] = Json::Int64{ message.CreatedAtUnixMilliseconds };
                encoded["updatedAt"] = Json::Int64{ message.UpdatedAtUnixMilliseconds };
                result["messages"].append(std::move(encoded));
            }
            result["events"] = Json::Value{ Json::arrayValue };
            for (const auto& event : value.Events)
            {
                Json::Value encoded{ Json::objectValue };
                encoded["id"] = event.Id;
                encoded["runId"] = _optional(event.RunId);
                encoded["aggregateSequence"] = Json::UInt64{ event.AggregateSequence };
                encoded["runSequence"] = _optional(event.RunSequence);
                encoded["type"] = event.Type;
                encoded["payloadJson"] = event.PayloadJson;
                encoded["createdAt"] = Json::Int64{ event.CreatedAtUnixMilliseconds };
                result["events"].append(std::move(encoded));
            }
            result["lastEventSequence"] = Json::UInt64{ value.LastEventSequence };
            return result;
        }

        bool _decodeExecution(Json::Value value, TaskProjectionExecutionRecord& result)
        {
            if (value.isMember("agent"))
            {
                if (!_readAgentSnapshot(value["agent"], result.Agent, result.AgentName)) return false;
                value.removeMember("agent");
            }
            if (!_exact(value, { "id", "revision", "mode", "sourceRoot", "runtimeRoot", "worktreePath", "branch", "relativeWorkingDirectory", "provider", "model", "instructions" }) ||
                !value["id"].isString() || !_unsigned(value["revision"]) || !_unsigned(value["mode"]) ||
                !value["sourceRoot"].isString() || !value["runtimeRoot"].isString() ||
                !value["worktreePath"].isString() || !value["branch"].isString() ||
                !value["relativeWorkingDirectory"].isString() || !value["provider"].isString() ||
                !value["model"].isString() || !value["instructions"].isString()) return false;
            result.Id = value["id"].asString();
            result.Revision = value["revision"].asUInt64();
            result.Mode = static_cast<TaskProjectionExecutionMode>(value["mode"].asUInt());
            result.SourceRoot = value["sourceRoot"].asString();
            result.RuntimeRoot = value["runtimeRoot"].asString();
            result.WorktreePath = value["worktreePath"].asString();
            result.Branch = value["branch"].asString();
            result.RelativeWorkingDirectory = value["relativeWorkingDirectory"].asString();
            result.Provider = value["provider"].asString();
            result.Model = value["model"].asString();
            result.Instructions = value["instructions"].asString();
            return true;
        }

        bool _decodeTask(const Json::Value& value, TaskProjectionRecord& result)
        {
            if (!_exact(value, { "id", "workspaceId", "title", "description", "acceptance", "status", "priority", "version", "manualHold", "createdAt", "updatedAt", "execution", "terminal", "agentSessions", "runs", "messages", "events", "lastEventSequence" }) ||
                !value["id"].isString() || !value["workspaceId"].isString() || !value["title"].isString() ||
                !value["description"].isString() || !value["acceptance"].isArray() || !_unsigned(value["status"]) ||
                !_unsigned(value["priority"]) || !_unsigned(value["version"]) || !value["manualHold"].isBool() ||
                !value["createdAt"].isInt64() || !value["updatedAt"].isInt64() ||
                !value["agentSessions"].isArray() || !value["runs"].isArray() || !value["messages"].isArray() ||
                !value["events"].isArray() || !_unsigned(value["lastEventSequence"]) ||
                value["acceptance"].size() > MaximumTaskProjectionAcceptance || value["runs"].size() > MaximumTaskProjectionRuns ||
                value["messages"].size() > MaximumTaskProjectionMessages || value["events"].size() > MaximumTaskProjectionEvents ||
                !_decodeExecution(value["execution"], result.Execution) ||
                !_exact(value["terminal"], { "state", "generation", "currentAgentSessionId" }) ||
                !_unsigned(value["terminal"]["state"]) || !_unsigned(value["terminal"]["generation"]) ||
                !_optionalString(value["terminal"]["currentAgentSessionId"], result.Terminal.CurrentAgentSessionId)) return false;
            result.Id = value["id"].asString();
            result.WorkspaceId = value["workspaceId"].asString();
            result.Title = value["title"].asString();
            result.Description = value["description"].asString();
            result.Status = static_cast<TaskProjectionStatus>(value["status"].asUInt());
            result.Priority = static_cast<TaskProjectionPriority>(value["priority"].asUInt());
            result.Version = value["version"].asUInt64();
            result.ManualHold = value["manualHold"].asBool();
            result.CreatedAtUnixMilliseconds = value["createdAt"].asInt64();
            result.UpdatedAtUnixMilliseconds = value["updatedAt"].asInt64();
            result.Terminal.State = static_cast<TaskProjectionTerminalState>(value["terminal"]["state"].asUInt());
            result.Terminal.Generation = value["terminal"]["generation"].asUInt64();
            result.LastEventSequence = value["lastEventSequence"].asUInt64();
            for (const auto& item : value["acceptance"])
            {
                if (!_exact(item, { "text", "satisfied" }) || !item["text"].isString() || !item["satisfied"].isBool()) return false;
                result.Acceptance.push_back({ item["text"].asString(), item["satisfied"].asBool() });
            }
            for (const auto& item : value["agentSessions"])
            {
                if (!_exact(item, { "id", "provider", "providerSessionId", "state", "isCurrent" }) ||
                    !item["id"].isString() || !item["provider"].isString() || !item["providerSessionId"].isString() ||
                    !_unsigned(item["state"]) || !item["isCurrent"].isBool()) return false;
                result.AgentSessions.push_back({ item["id"].asString(), item["provider"].asString(), item["providerSessionId"].asString(), static_cast<uint8_t>(item["state"].asUInt()), item["isCurrent"].asBool() });
            }
            for (auto item : value["runs"])
            {
                TaskProjectionRunRecord run;
                if (item.isMember("executionAgent"))
                {
                    auto execution = item["executionAgent"];
                    if (!execution.isObject() || !execution["provider"].isString() || !execution["model"].isString() ||
                        !_unsigned(execution["contextRevision"]) || !execution["contextRevision"].asUInt64() || execution["contextRevision"].asUInt64() > INT64_MAX) return false;
                    run.Provider = execution["provider"].asString(); run.Model = execution["model"].asString(); run.ExecutionRevision = execution["contextRevision"].asUInt64();
                    execution.removeMember("provider"); execution.removeMember("model"); execution.removeMember("contextRevision");
                    if (!_readAgentSnapshot(execution, run.Agent, run.AgentName)) return false;
                    item.removeMember("executionAgent");
                }
                if (!_exact(item, { "id", "executionContextId", "agentSessionId", "retryOfRunId", "trigger", "state", "sequence", "createdAt", "startedAt", "finishedAt", "result", "reasonCode" }) ||
                    !item["id"].isString() || !item["executionContextId"].isString() || !_unsigned(item["trigger"]) ||
                    !_unsigned(item["state"]) || !_unsigned(item["sequence"]) || !item["createdAt"].isInt64() ||
                    (!item["startedAt"].isNull() && !item["startedAt"].isInt64()) ||
                    (!item["finishedAt"].isNull() && !item["finishedAt"].isInt64()) ||
                    !item["result"].isString() || !item["reasonCode"].isString()) return false;
                run.Id = item["id"].asString();
                run.ExecutionContextId = item["executionContextId"].asString();
                if (!_optionalString(item["agentSessionId"], run.AgentSessionId) || !_optionalString(item["retryOfRunId"], run.RetryOfRunId)) return false;
                run.Trigger = static_cast<uint8_t>(item["trigger"].asUInt());
                run.State = static_cast<TaskProjectionRunState>(item["state"].asUInt());
                run.Sequence = item["sequence"].asUInt64();
                run.CreatedAtUnixMilliseconds = item["createdAt"].asInt64();
                if (!item["startedAt"].isNull()) run.StartedAtUnixMilliseconds = item["startedAt"].asInt64();
                if (!item["finishedAt"].isNull()) run.FinishedAtUnixMilliseconds = item["finishedAt"].asInt64();
                run.Result = item["result"].asString();
                run.ReasonCode = item["reasonCode"].asString();
                result.Runs.emplace_back(std::move(run));
            }
            for (const auto& item : value["messages"])
            {
                if (!_exact(item, { "id", "author", "state", "revision", "content", "claimedByRunId", "createdAt", "updatedAt" }) ||
                    !item["id"].isString() || !_unsigned(item["author"]) || !_unsigned(item["state"]) ||
                    !_unsigned(item["revision"]) || !item["content"].isString() ||
                    !item["createdAt"].isInt64() || !item["updatedAt"].isInt64()) return false;
                TaskProjectionMessageRecord message;
                message.Id = item["id"].asString();
                message.Author = static_cast<uint8_t>(item["author"].asUInt());
                message.State = static_cast<uint8_t>(item["state"].asUInt());
                message.Revision = item["revision"].asUInt64();
                message.Content = item["content"].asString();
                if (!_optionalString(item["claimedByRunId"], message.ClaimedByRunId)) return false;
                message.CreatedAtUnixMilliseconds = item["createdAt"].asInt64();
                message.UpdatedAtUnixMilliseconds = item["updatedAt"].asInt64();
                result.Messages.emplace_back(std::move(message));
            }
            for (const auto& item : value["events"])
            {
                if (!_exact(item, { "id", "runId", "aggregateSequence", "runSequence", "type", "payloadJson", "createdAt" }) ||
                    !item["id"].isString() || !_unsigned(item["aggregateSequence"]) ||
                    (!item["runSequence"].isNull() && !_unsigned(item["runSequence"])) ||
                    !item["type"].isString() || !item["payloadJson"].isString() || !item["createdAt"].isInt64()) return false;
                TaskProjectionEventRecord event;
                event.Id = item["id"].asString();
                if (!_optionalString(item["runId"], event.RunId)) return false;
                event.AggregateSequence = item["aggregateSequence"].asUInt64();
                if (!item["runSequence"].isNull()) event.RunSequence = item["runSequence"].asUInt64();
                event.Type = item["type"].asString();
                event.PayloadJson = item["payloadJson"].asString();
                event.CreatedAtUnixMilliseconds = item["createdAt"].asInt64();
                result.Events.emplace_back(std::move(event));
            }
            std::string error;
            return ValidateTaskProjection(result, error);
        }
    }

    ProtocolRpcJsonDecodeResult<TaskProjectionQueryRequest> DecodeTaskProjectionQueryRequest(const std::string_view payload)
    {
        Json::Value root;
        ProtocolRpcJsonDecodeResult<TaskProjectionQueryRequest> result;
        if (!_decodeRoot(payload, root, result)) return result;
        if (!_exact(root, { "taskId", "workspaceId", "includeCompleted", "eventLimit" }) ||
            !root["includeCompleted"].isBool() || !_unsigned(root["eventLimit"]) ||
            !_optionalString(root["taskId"], result.Value.TaskId) ||
            !_optionalString(root["workspaceId"], result.Value.WorkspaceId)) return _failure<TaskProjectionQueryRequest>("Task projection query schema is invalid");
        result.Value.IncludeCompleted = root["includeCompleted"].asBool();
        result.Value.EventLimit = root["eventLimit"].asUInt64();
        std::string error;
        if (!ValidateTaskProjectionQuery(result.Value, error)) return _failure<TaskProjectionQueryRequest>(std::move(error));
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeTaskProjectionQueryRequest(const TaskProjectionQueryRequest& value)
    {
        std::string error;
        if (!ValidateTaskProjectionQuery(value, error)) return { false, {}, std::move(error) };
        Json::Value root{ Json::objectValue };
        root["taskId"] = _optional(value.TaskId);
        root["workspaceId"] = _optional(value.WorkspaceId);
        root["includeCompleted"] = value.IncludeCompleted;
        root["eventLimit"] = Json::UInt64{ value.EventLimit };
        return _encode(std::move(root));
    }

    ProtocolRpcJsonDecodeResult<TaskProjectionQueryResult> DecodeTaskProjectionQueryResult(const std::string_view payload)
    {
        Json::Value root;
        ProtocolRpcJsonDecodeResult<TaskProjectionQueryResult> result;
        if (!_decodeRoot(payload, root, result)) return result;
        if (!_exact(root, { "generation", "tasks" }) || !_unsigned(root["generation"]) ||
            !root["tasks"].isArray() || root["tasks"].size() > MaximumTaskProjectionItems) return _failure<TaskProjectionQueryResult>("Task projection result schema is invalid");
        result.Value.Generation = root["generation"].asUInt64();
        result.Value.Tasks.reserve(root["tasks"].size());
        for (const auto& item : root["tasks"])
        {
            TaskProjectionRecord task;
            if (!_decodeTask(item, task)) return _failure<TaskProjectionQueryResult>("Task projection item schema is invalid");
            result.Value.Tasks.emplace_back(std::move(task));
        }
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeTaskProjectionQueryResult(const TaskProjectionQueryResult& value, const bool includeAgentProfiles)
    {
        if (value.Tasks.size() > MaximumTaskProjectionItems) return { false, {}, "Task projection result exceeds item capacity" };
        Json::Value root{ Json::objectValue };
        root["generation"] = Json::UInt64{ value.Generation };
        root["tasks"] = Json::Value{ Json::arrayValue };
        for (const auto& task : value.Tasks)
        {
            std::string error;
            if (!ValidateTaskProjection(task, error)) return { false, {}, std::move(error) };
            root["tasks"].append(_task(task, includeAgentProfiles));
        }
        return _encode(std::move(root));
    }
}
