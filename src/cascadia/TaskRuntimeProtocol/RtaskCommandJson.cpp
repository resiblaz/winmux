// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RtaskCommandJson.h"

#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

#include <json/json.h>

#include <memory>

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

        bool _parse(const std::string_view payload, Json::Value& root, std::string& error)
        {
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false;
            builder["allowTrailingCommas"] = false;
            builder["collectComments"] = false;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            builder["strictRoot"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            return reader->parse(
                payload.data(),
                payload.data() + payload.size(),
                &root,
                &error);
        }

        bool _exact(
            const Json::Value& value,
            const std::initializer_list<std::string_view> names)
        {
            return value.isObject() && value.size() == names.size() &&
                   std::ranges::all_of(names, [&](const auto name) {
                       return value.isMember(std::string{ name });
                   });
        }

        std::string _write(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            builder["indentation"] = "";
            return Json::writeString(builder, value);
        }

        bool _isUnsignedInteger(const Json::Value& value) noexcept
        {
            return value.type() == Json::uintValue ||
                   (value.type() == Json::intValue && value.asInt64() >= 0);
        }

        template<typename TValue>
        bool _decodeRoot(
            const std::string_view payload,
            Json::Value& root,
            ProtocolRpcJsonDecodeResult<TValue>& failure)
        {
            if (payload.size() > MaximumControlFrameBytes)
            {
                failure = _failure<TValue>(
                    ProtocolRpcJsonStatus::Oversized,
                    "rtask JSON exceeds the control payload limit");
                return false;
            }
            if (!IsValidUtf8(payload))
            {
                failure = _failure<TValue>(
                    ProtocolRpcJsonStatus::InvalidUtf8,
                    "rtask JSON is not valid UTF-8");
                return false;
            }
            std::string error;
            if (!_parse(payload, root, error))
            {
                failure = _failure<TValue>(
                    ProtocolRpcJsonStatus::MalformedJson,
                    "rtask JSON is malformed");
                return false;
            }
            if (!root.isObject())
            {
                failure = _failure<TValue>(
                    ProtocolRpcJsonStatus::InvalidSchema,
                    "rtask JSON root must be an object");
                return false;
            }
            return true;
        }

        ProtocolRpcJsonEncodeResult _encoded(Json::Value root)
        {
            ProtocolRpcJsonEncodeResult result;
            result.Payload = _write(root);
            if (result.Payload.empty() ||
                result.Payload.size() > MaximumControlFrameBytes ||
                !IsValidUtf8(result.Payload))
            {
                result.Payload.clear();
                result.Error = "Encoded rtask JSON exceeds protocol bounds";
                return result;
            }
            result.Succeeded = true;
            return result;
        }
    }

    ProtocolRpcJsonDecodeResult<RtaskCommandRequest> DecodeRtaskCommandRequest(
        const std::string_view payload)
    {
        Json::Value root;
        ProtocolRpcJsonDecodeResult<RtaskCommandRequest> failure;
        if (!_decodeRoot(payload, root, failure))
        {
            return failure;
        }
        if (!root["commandId"].isString() || !root["runId"].isString() ||
            !root["capability"].isString() || !root["method"].isString())
        {
            return _failure<RtaskCommandRequest>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "rtask command fields are invalid");
        }

        const auto method = RtaskMethodFromName(root["method"].asString());
        if (!method)
        {
            return _failure<RtaskCommandRequest>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "rtask command method is invalid");
        }
        const auto acceptsText = *method == RtaskMethod::StatusBlocked ||
                                 *method == RtaskMethod::Result;
        if ((acceptsText &&
             (!_exact(root, { "commandId", "runId", "capability", "method", "text" }) ||
              !root["text"].isString())) ||
            (!acceptsText &&
             !_exact(root, { "commandId", "runId", "capability", "method" })))
        {
            return _failure<RtaskCommandRequest>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "rtask command schema is invalid for its method");
        }

        RtaskCommandRequest request;
        request.CommandId = root["commandId"].asString();
        request.RunId = root["runId"].asString();
        request.Capability = root["capability"].asString();
        request.Method = *method;
        if (acceptsText)
        {
            request.Text = root["text"].asString();
        }
        std::string error;
        if (!ValidateRtaskCommand(request, error))
        {
            return _failure<RtaskCommandRequest>(
                ProtocolRpcJsonStatus::InvalidSchema,
                std::move(error));
        }

        ProtocolRpcJsonDecodeResult<RtaskCommandRequest> result;
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        result.Value = std::move(request);
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeRtaskCommandRequest(
        const RtaskCommandRequest& request)
    {
        ProtocolRpcJsonEncodeResult result;
        std::string error;
        if (!ValidateRtaskCommand(request, error))
        {
            result.Error = std::move(error);
            return result;
        }
        Json::Value root{ Json::objectValue };
        root["commandId"] = request.CommandId;
        root["runId"] = request.RunId;
        root["capability"] = request.Capability;
        root["method"] = std::string{ RtaskMethodName(request.Method) };
        if (request.Method == RtaskMethod::StatusBlocked ||
            request.Method == RtaskMethod::Result)
        {
            root["text"] = request.Text;
        }
        return _encoded(std::move(root));
    }

    ProtocolRpcJsonDecodeResult<RtaskCommandResult> DecodeRtaskCommandResult(
        const std::string_view payload)
    {
        Json::Value root;
        ProtocolRpcJsonDecodeResult<RtaskCommandResult> failure;
        if (!_decodeRoot(payload, root, failure))
        {
            return failure;
        }
        if (!_exact(
                root,
                { "commandId", "disposition", "taskVersion", "eventSequence" }) ||
            !root["commandId"].isString() || !root["disposition"].isString() ||
            !_isUnsignedInteger(root["taskVersion"]) ||
            !_isUnsignedInteger(root["eventSequence"]))
        {
            return _failure<RtaskCommandResult>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "rtask command result fields are invalid");
        }

        const auto disposition = RtaskCommandDispositionFromName(
            root["disposition"].asString());
        RtaskCommandResult commandResult;
        commandResult.CommandId = root["commandId"].asString();
        commandResult.Disposition = disposition.value_or(
            RtaskCommandDisposition::Count);
        commandResult.TaskVersion = root["taskVersion"].asUInt64();
        commandResult.EventSequence = root["eventSequence"].asUInt64();
        std::string error;
        if (!disposition || !ValidateRtaskCommandResult(commandResult, error))
        {
            return _failure<RtaskCommandResult>(
                ProtocolRpcJsonStatus::InvalidSchema,
                std::move(error));
        }

        ProtocolRpcJsonDecodeResult<RtaskCommandResult> result;
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        result.Value = std::move(commandResult);
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeRtaskCommandResult(
        const RtaskCommandResult& result)
    {
        ProtocolRpcJsonEncodeResult encoded;
        std::string error;
        if (!ValidateRtaskCommandResult(result, error))
        {
            encoded.Error = std::move(error);
            return encoded;
        }
        Json::Value root{ Json::objectValue };
        root["commandId"] = result.CommandId;
        root["disposition"] = std::string{
            RtaskCommandDispositionName(result.Disposition)
        };
        root["taskVersion"] = Json::UInt64{ result.TaskVersion };
        root["eventSequence"] = Json::UInt64{ result.EventSequence };
        return _encoded(std::move(root));
    }
}
