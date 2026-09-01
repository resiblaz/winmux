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
            return result;
        }

        bool _decodeDraft(const Json::Value& value, TaskUiCreateDraft& draft)
        {
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
    }

    ProtocolRpcJsonDecodeResult<TaskUiCommandRequest> DecodeTaskUiCommandRequest(const std::string_view payload)
    {
        Json::Value root;
        ProtocolRpcJsonDecodeResult<TaskUiCommandRequest> result;
        if (!_root(payload, root, result)) return result;
        if (!_exact(root, { "commandId", "operation", "taskId", "expectedTaskVersion", "draft" }) ||
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
        return _encode(std::move(root));
    }

    ProtocolRpcJsonDecodeResult<TaskUiCommandResult> DecodeTaskUiCommandResult(const std::string_view payload)
    {
        Json::Value root;
        ProtocolRpcJsonDecodeResult<TaskUiCommandResult> result;
        if (!_root(payload, root, result)) return result;
        if (!_exact(root, { "commandId", "taskId", "runId", "taskVersion", "eventSequence", "replayed", "pending" }) ||
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
        return _encode(std::move(root));
    }
}
