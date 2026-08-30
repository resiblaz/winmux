// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeFoundationRpc.h"

#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

#include <json/json.h>

#include <limits>
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
            return reader && reader->parse(
                                 payload.data(),
                                 payload.data() + payload.size(),
                                 &root,
                                 &error);
        }

        bool _decodeRoot(
            const std::string_view payload,
            Json::Value& root,
            ProtocolRpcJsonStatus& status,
            std::string& error)
        {
            if (payload.size() > MaximumControlFrameBytes)
            {
                status = ProtocolRpcJsonStatus::Oversized;
                error = "Runtime foundation RPC JSON exceeds the control payload limit";
                return false;
            }
            if (!IsValidUtf8(payload))
            {
                status = ProtocolRpcJsonStatus::InvalidUtf8;
                error = "Runtime foundation RPC JSON is not valid UTF-8";
                return false;
            }
            if (!_parse(payload, root, error))
            {
                status = ProtocolRpcJsonStatus::MalformedJson;
                return false;
            }
            return true;
        }

        bool _exact(const Json::Value& value, const std::initializer_list<std::string_view> names)
        {
            return value.isObject() && value.size() == names.size() &&
                   std::ranges::all_of(names, [&](const auto name) {
                       return value.isMember(std::string{ name });
                   });
        }

        bool _isUnsignedInteger(const Json::Value& value) noexcept
        {
            return value.type() == Json::uintValue ||
                   (value.type() == Json::intValue && value.asInt64() >= 0);
        }

        std::string _write(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            builder["indentation"] = "";
            return Json::writeString(builder, value);
        }

        ProtocolRpcJsonEncodeResult _encoded(const Json::Value& root)
        {
            ProtocolRpcJsonEncodeResult result;
            result.Payload = _write(root);
            if (result.Payload.empty() || result.Payload.size() > MaximumControlFrameBytes ||
                !IsValidUtf8(result.Payload))
            {
                result.Payload.clear();
                result.Error = "Encoded Runtime foundation RPC JSON exceeds protocol bounds";
                return result;
            }
            result.Succeeded = true;
            return result;
        }

        bool _validPingResult(const PingResult& result) noexcept
        {
            return IsCanonicalProtocolUuid(result.RuntimeInstanceId) &&
                   IsCanonicalProtocolUuid(result.ClientInstanceId);
        }

        bool _validRuntimeStatusResult(const RuntimeStatusResult& result) noexcept
        {
            return IsValidRuntimeIdentity(result.RuntimeIdentity) &&
                   IsCanonicalProtocolUuid(result.RuntimeInstanceId) &&
                   result.ProcessId != 0 && result.State == RuntimeFoundationReadyState;
        }
    }

    ProtocolRpcJsonDecodeResult<PingResult> DecodePingResult(
        const std::string_view resultJson)
    {
        Json::Value root;
        std::string error;
        ProtocolRpcJsonStatus status{ ProtocolRpcJsonStatus::InvalidSchema };
        if (!_decodeRoot(resultJson, root, status, error))
        {
            return _failure<PingResult>(status, std::move(error));
        }
        if (!_exact(root, { "runtimeInstanceId", "clientInstanceId" }) ||
            !root["runtimeInstanceId"].isString() || !root["clientInstanceId"].isString())
        {
            return _failure<PingResult>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "Ping result fields are invalid");
        }

        PingResult value;
        value.RuntimeInstanceId = root["runtimeInstanceId"].asString();
        value.ClientInstanceId = root["clientInstanceId"].asString();
        if (!_validPingResult(value))
        {
            return _failure<PingResult>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "Ping result identity is invalid");
        }
        ProtocolRpcJsonDecodeResult<PingResult> result;
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        result.Value = std::move(value);
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodePingResult(const PingResult& result)
    {
        ProtocolRpcJsonEncodeResult encoded;
        if (!_validPingResult(result))
        {
            encoded.Error = "Ping result is invalid";
            return encoded;
        }
        Json::Value root{ Json::objectValue };
        root["runtimeInstanceId"] = result.RuntimeInstanceId;
        root["clientInstanceId"] = result.ClientInstanceId;
        return _encoded(root);
    }

    ProtocolRpcJsonDecodeResult<RuntimeStatusResult> DecodeRuntimeStatusResult(
        const std::string_view resultJson)
    {
        Json::Value root;
        std::string error;
        ProtocolRpcJsonStatus status{ ProtocolRpcJsonStatus::InvalidSchema };
        if (!_decodeRoot(resultJson, root, status, error))
        {
            return _failure<RuntimeStatusResult>(status, std::move(error));
        }
        if (!_exact(root, { "runtimeIdentity", "runtimeInstanceId", "processId", "state" }) ||
            !root["runtimeIdentity"].isString() || !root["runtimeInstanceId"].isString() ||
            !_isUnsignedInteger(root["processId"]) || !root["state"].isString())
        {
            return _failure<RuntimeStatusResult>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "RuntimeStatus result fields are invalid");
        }

        const auto processId = root["processId"].asUInt64();
        if (processId == 0 || processId > std::numeric_limits<uint32_t>::max())
        {
            return _failure<RuntimeStatusResult>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "RuntimeStatus process identity is invalid");
        }
        RuntimeStatusResult value;
        value.RuntimeIdentity = root["runtimeIdentity"].asString();
        value.RuntimeInstanceId = root["runtimeInstanceId"].asString();
        value.ProcessId = static_cast<uint32_t>(processId);
        value.State = root["state"].asString();
        if (!_validRuntimeStatusResult(value))
        {
            return _failure<RuntimeStatusResult>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "RuntimeStatus result values are invalid");
        }
        ProtocolRpcJsonDecodeResult<RuntimeStatusResult> result;
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        result.Value = std::move(value);
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeRuntimeStatusResult(
        const RuntimeStatusResult& result)
    {
        ProtocolRpcJsonEncodeResult encoded;
        if (!_validRuntimeStatusResult(result))
        {
            encoded.Error = "RuntimeStatus result is invalid";
            return encoded;
        }
        Json::Value root{ Json::objectValue };
        root["runtimeIdentity"] = result.RuntimeIdentity;
        root["runtimeInstanceId"] = result.RuntimeInstanceId;
        root["processId"] = Json::UInt{ result.ProcessId };
        root["state"] = result.State;
        return _encoded(root);
    }
}
