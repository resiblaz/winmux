// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProjectionRpc.h"
#include "AgentCatalogJson.h"

#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

#include <json/json.h>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        template<typename TValue>
        ProtocolRpcJsonDecodeResult<TValue> _failure(
            const ProtocolRpcJsonStatus status,
            std::string error)
        {
            ProtocolRpcJsonDecodeResult<TValue> result;
            result.Status = status;
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

        template<typename TValue>
        bool _root(const std::string_view payload, Json::Value& value, ProtocolRpcJsonDecodeResult<TValue>& failure)
        {
            if (payload.size() > MaximumControlFrameBytes || !IsValidUtf8(payload))
            {
                failure = _failure<TValue>(
                    payload.size() > MaximumControlFrameBytes ? ProtocolRpcJsonStatus::Oversized : ProtocolRpcJsonStatus::InvalidUtf8,
                    "Task command JSON exceeds protocol bounds");
                return false;
            }
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false;
            builder["allowTrailingCommas"] = false;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            std::string error;
            if (!reader || !reader->parse(payload.data(), payload.data() + payload.size(), &value, &error) || !value.isObject())
            {
                failure = _failure<TValue>(ProtocolRpcJsonStatus::MalformedJson, "Task command JSON is malformed");
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
                result.Error = "Encoded Task command exceeds protocol bounds";
                return result;
            }
            result.Succeeded = true;
            return result;
        }

        Json::Value _draft(const TaskUiCreateDraft& draft)
        {
            Json::Value result{ Json::objectValue };
            result["workspaceId"] = draft.WorkspaceId;
            result["title"] = draft.Title;
            result["description"] = draft.Description;
            result["acceptance"] = Json::Value{ Json::arrayValue };
            for (const auto& item : draft.Acceptance) result["acceptance"].append(item);
            result["priority"] = static_cast<uint32_t>(draft.Priority);
            result["executionMode"] = static_cast<uint32_t>(draft.ExecutionMode);
            result["sourceRoot"] = draft.SourceRoot;
            result["relativeWorkingDirectory"] = draft.RelativeWorkingDirectory;
            result["provider"] = draft.Provider;
            result["model"] = draft.Model;
            result["instructions"] = draft.Instructions;
            if (draft.ProjectVersion) result["projectVersion"] = Json::UInt64{ draft.ProjectVersion };
            if (draft.Agent)
            {
                result["agentId"] = draft.Agent->Id;
                result["agentRevision"] = Json::UInt64{ draft.Agent->Revision };
            }
            return result;
        }

        bool _decodeDraft(Json::Value value, TaskUiCreateDraft& draft)
        {
            if (value.isMember("projectVersion"))
            {
                if (!_unsigned(value["projectVersion"]) || !value["projectVersion"].asUInt64() || value["projectVersion"].asUInt64() > INT64_MAX) return false;
                draft.ProjectVersion = value["projectVersion"].asUInt64(); value.removeMember("projectVersion");
            }
            if (value.isMember("agentId") || value.isMember("agentRevision"))
            {
                if (!value["agentId"].isString() || !_unsigned(value["agentRevision"])) return false;
                draft.Agent = AgentProfileReference{ value["agentId"].asString(), value["agentRevision"].asUInt64() };
                value.removeMember("agentId"); value.removeMember("agentRevision");
            }
            if (!_exact(value, { "workspaceId", "title", "description", "acceptance", "priority", "executionMode", "sourceRoot", "relativeWorkingDirectory", "provider", "model", "instructions" }) ||
                !value["workspaceId"].isString() || !value["title"].isString() || !value["description"].isString() ||
                !value["acceptance"].isArray() || value["acceptance"].size() > MaximumTaskProjectionAcceptance ||
                !_unsigned(value["priority"]) || !_unsigned(value["executionMode"]) || !value["sourceRoot"].isString() ||
                !value["relativeWorkingDirectory"].isString() || !value["provider"].isString() ||
                !value["model"].isString() || !value["instructions"].isString()) return false;
            draft.WorkspaceId = value["workspaceId"].asString();
            draft.Title = value["title"].asString();
            draft.Description = value["description"].asString();
            for (const auto& item : value["acceptance"])
            {
                if (!item.isString()) return false;
                draft.Acceptance.emplace_back(item.asString());
            }
            draft.Priority = static_cast<TaskProjectionPriority>(value["priority"].asUInt());
            draft.ExecutionMode = static_cast<TaskProjectionExecutionMode>(value["executionMode"].asUInt());
            draft.SourceRoot = value["sourceRoot"].asString();
            draft.RelativeWorkingDirectory = value["relativeWorkingDirectory"].asString();
            draft.Provider = value["provider"].asString();
            draft.Model = value["model"].asString();
            draft.Instructions = value["instructions"].asString();
            return true;
        }

        Json::Value _arguments(const TaskUiCommandRequest& value)
        {
            Json::Value result{ Json::objectValue };
            result["messageId"] = value.MessageId ? Json::Value{ *value.MessageId } : Json::Value{};
            result["expectedMessageRevision"] = value.ExpectedMessageRevision ?
                                                     Json::Value{ Json::UInt64{ *value.ExpectedMessageRevision } } :
                                                     Json::Value{};
            result["text"] = value.Text ? Json::Value{ *value.Text } : Json::Value{};
            result["relatedRunId"] = value.RelatedRunId ? Json::Value{ *value.RelatedRunId } : Json::Value{};
            result["derivedTaskId"] = value.DerivedTaskId ? Json::Value{ *value.DerivedTaskId } : Json::Value{};
            if (value.AgentConfiguration) result["agentConfiguration"] = details::TaskAgentConfigurationJson(*value.AgentConfiguration);
            return result;
        }

        bool _decodeArguments(Json::Value value, TaskUiCommandRequest& request)
        {
            if (value.isMember("agentConfiguration"))
            {
                request.AgentConfiguration.emplace();
                if (!details::ReadTaskAgentConfigurationJson(value["agentConfiguration"], *request.AgentConfiguration)) return false;
                value.removeMember("agentConfiguration");
            }
            if (!_exact(value, { "messageId", "expectedMessageRevision", "text", "relatedRunId", "derivedTaskId" }) ||
                (!value["messageId"].isNull() && !value["messageId"].isString()) ||
                (!value["expectedMessageRevision"].isNull() && !_unsigned(value["expectedMessageRevision"])) ||
                (!value["text"].isNull() && !value["text"].isString()) ||
                (!value["relatedRunId"].isNull() && !value["relatedRunId"].isString()) ||
                (!value["derivedTaskId"].isNull() && !value["derivedTaskId"].isString()))
            {
                return false;
            }
            if (value["messageId"].isString()) request.MessageId = value["messageId"].asString();
            if (!value["expectedMessageRevision"].isNull()) request.ExpectedMessageRevision = value["expectedMessageRevision"].asUInt64();
            if (value["text"].isString()) request.Text = value["text"].asString();
            if (value["relatedRunId"].isString()) request.RelatedRunId = value["relatedRunId"].asString();
            if (value["derivedTaskId"].isString()) request.DerivedTaskId = value["derivedTaskId"].asString();
            return true;
        }

        Json::Value _surface(const TaskUiSurfaceDescriptor& value)
        {
            Json::Value result{ Json::objectValue };
            result["terminalSessionId"] = value.TerminalSessionId;
            result["pipeName"] = value.PipeName;
            result["generation"] = Json::UInt64{ value.Generation };
            result["ownedElsewhere"] = value.OwnedElsewhere;
            return result;
        }

        bool _decodeSurface(const Json::Value& value, TaskUiSurfaceDescriptor& surface)
        {
            if (!_exact(value, { "terminalSessionId", "pipeName", "generation", "ownedElsewhere" }) ||
                !value["terminalSessionId"].isString() || !value["pipeName"].isString() || !_unsigned(value["generation"]) ||
                !value["ownedElsewhere"].isBool())
            {
                return false;
            }
            surface.TerminalSessionId = value["terminalSessionId"].asString();
            surface.PipeName = value["pipeName"].asString();
            surface.Generation = value["generation"].asUInt64();
            surface.OwnedElsewhere = value["ownedElsewhere"].asBool();
            return IsCanonicalProtocolUuid(surface.TerminalSessionId) &&
                   !surface.PipeName.empty() && surface.PipeName.size() <= 512 &&
                   IsValidUtf8(surface.PipeName) && surface.Generation != 0;
        }
    }

    ProtocolRpcJsonDecodeResult<TaskUiCommandRequest> DecodeTaskUiCommandRequest(const std::string_view payload)
    {
        Json::Value root;
        ProtocolRpcJsonDecodeResult<TaskUiCommandRequest> result;
        if (!_root(payload, root, result)) return result;
        const auto legacy = _exact(root, { "commandId", "operation", "taskId", "expectedTaskVersion", "draft" });
        const auto extended = _exact(root, { "commandId", "operation", "taskId", "expectedTaskVersion", "draft", "arguments" });
        if ((!legacy && !extended) ||
            !root["commandId"].isString() || !root["operation"].isString() || !root["taskId"].isString() ||
            !_unsigned(root["expectedTaskVersion"])) return _failure<TaskUiCommandRequest>(ProtocolRpcJsonStatus::InvalidSchema, "Task command envelope is invalid");
        const auto operation = TaskUiCommandOperationFromName(root["operation"].asString());
        if (!operation) return _failure<TaskUiCommandRequest>(ProtocolRpcJsonStatus::InvalidSchema, "Task command operation is invalid");
        result.Value.CommandId = root["commandId"].asString();
        result.Value.Operation = *operation;
        result.Value.TaskId = root["taskId"].asString();
        result.Value.ExpectedTaskVersion = root["expectedTaskVersion"].asUInt64();
        if (!root["draft"].isNull())
        {
            TaskUiCreateDraft draft;
            if (!_decodeDraft(root["draft"], draft)) return _failure<TaskUiCommandRequest>(ProtocolRpcJsonStatus::InvalidSchema, "Task create draft schema is invalid");
            result.Value.Draft = std::move(draft);
        }
        if (extended && !_decodeArguments(root["arguments"], result.Value))
        {
            return _failure<TaskUiCommandRequest>(ProtocolRpcJsonStatus::InvalidSchema, "Task command arguments schema is invalid");
        }
        std::string error;
        if (!ValidateTaskUiCommand(result.Value, error)) return _failure<TaskUiCommandRequest>(ProtocolRpcJsonStatus::InvalidSchema, std::move(error));
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeTaskUiCommandRequest(const TaskUiCommandRequest& value)
    {
        std::string error;
        if (!ValidateTaskUiCommand(value, error)) return { false, {}, std::move(error) };
        Json::Value root{ Json::objectValue };
        root["commandId"] = value.CommandId;
        root["operation"] = std::string{ TaskUiCommandOperationName(value.Operation) };
        root["taskId"] = value.TaskId;
        root["expectedTaskVersion"] = Json::UInt64{ value.ExpectedTaskVersion };
        root["draft"] = value.Draft ? _draft(*value.Draft) : Json::Value{};
        if (value.MessageId || value.ExpectedMessageRevision || value.Text || value.RelatedRunId || value.DerivedTaskId || value.AgentConfiguration)
        {
            root["arguments"] = _arguments(value);
        }
        return _encode(std::move(root));
    }

    ProtocolRpcJsonDecodeResult<TaskUiCommandResult> DecodeTaskUiCommandResult(const std::string_view payload)
    {
        Json::Value root;
        ProtocolRpcJsonDecodeResult<TaskUiCommandResult> result;
        if (!_root(payload, root, result)) return result;
        const auto legacy = _exact(root, { "commandId", "taskId", "runId", "taskVersion", "eventSequence", "replayed", "pending" });
        const auto extended = _exact(root, { "commandId", "taskId", "runId", "taskVersion", "eventSequence", "replayed", "pending", "surface", "surfaceCloseAuthorized", "affectedTaskCount" });
        if ((!legacy && !extended) ||
            !root["commandId"].isString() || !root["taskId"].isString() ||
            (!root["runId"].isNull() && !root["runId"].isString()) || !_unsigned(root["taskVersion"]) ||
            !_unsigned(root["eventSequence"]) || !root["replayed"].isBool() || !root["pending"].isBool() ||
            !IsCanonicalProtocolUuid(root["commandId"].asString()) || !IsCanonicalProtocolUuid(root["taskId"].asString()) ||
            (root["runId"].isString() && !IsCanonicalProtocolUuid(root["runId"].asString())))
        {
            return _failure<TaskUiCommandResult>(ProtocolRpcJsonStatus::InvalidSchema, "Task command result schema is invalid");
        }
        result.Value.CommandId = root["commandId"].asString();
        result.Value.TaskId = root["taskId"].asString();
        if (root["runId"].isString()) result.Value.RunId = root["runId"].asString();
        result.Value.TaskVersion = root["taskVersion"].asUInt64();
        result.Value.EventSequence = root["eventSequence"].asUInt64();
        result.Value.Replayed = root["replayed"].asBool();
        result.Value.Pending = root["pending"].asBool();
        if (extended)
        {
            if ((!root["surface"].isNull() && !root["surface"].isObject()) ||
                !root["surfaceCloseAuthorized"].isBool() || !_unsigned(root["affectedTaskCount"]))
            {
                return _failure<TaskUiCommandResult>(ProtocolRpcJsonStatus::InvalidSchema, "Task command result extension is invalid");
            }
            if (root["surface"].isObject())
            {
                TaskUiSurfaceDescriptor surface;
                if (!_decodeSurface(root["surface"], surface))
                {
                    return _failure<TaskUiCommandResult>(ProtocolRpcJsonStatus::InvalidSchema, "Task Surface descriptor is invalid");
                }
                result.Value.Surface = std::move(surface);
            }
            result.Value.SurfaceCloseAuthorized = root["surfaceCloseAuthorized"].asBool();
            result.Value.AffectedTaskCount = root["affectedTaskCount"].asUInt();
        }
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeTaskUiCommandResult(const TaskUiCommandResult& value)
    {
        if (!IsCanonicalProtocolUuid(value.CommandId) || !IsCanonicalProtocolUuid(value.TaskId) ||
            (value.RunId && !IsCanonicalProtocolUuid(*value.RunId)) || value.TaskVersion == 0 || value.EventSequence == 0)
        {
            return { false, {}, "Task command result is invalid" };
        }
        Json::Value root{ Json::objectValue };
        root["commandId"] = value.CommandId;
        root["taskId"] = value.TaskId;
        root["runId"] = value.RunId ? Json::Value{ *value.RunId } : Json::Value{};
        root["taskVersion"] = Json::UInt64{ value.TaskVersion };
        root["eventSequence"] = Json::UInt64{ value.EventSequence };
        root["replayed"] = value.Replayed;
        root["pending"] = value.Pending;
        if (value.Surface || value.SurfaceCloseAuthorized || value.AffectedTaskCount != 0)
        {
            if (value.Surface && (!IsCanonicalProtocolUuid(value.Surface->TerminalSessionId) ||
                                  value.Surface->PipeName.empty() || value.Surface->PipeName.size() > 512 ||
                                  !IsValidUtf8(value.Surface->PipeName) || value.Surface->Generation == 0))
            {
                return { false, {}, "Task Surface descriptor is invalid" };
            }
            root["surface"] = value.Surface ? _surface(*value.Surface) : Json::Value{};
            root["surfaceCloseAuthorized"] = value.SurfaceCloseAuthorized;
            root["affectedTaskCount"] = value.AffectedTaskCount;
        }
        return _encode(std::move(root));
    }
}
