// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SnapshotAndSubscribe.h"

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
            return reader->parse(payload.data(), payload.data() + payload.size(), &root, &error);
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
                error = "Snapshot RPC JSON exceeds the control payload limit";
                return false;
            }
            if (!IsValidUtf8(payload))
            {
                status = ProtocolRpcJsonStatus::InvalidUtf8;
                error = "Snapshot RPC JSON is not valid UTF-8";
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
                result.Error = "Encoded snapshot RPC JSON exceeds protocol bounds";
                return result;
            }
            result.Succeeded = true;
            return result;
        }
    }

    ProtocolRpcJsonDecodeResult<SnapshotAndSubscribeResult> DecodeSnapshotAndSubscribeResult(
        const std::string_view resultJson)
    {
        Json::Value root;
        std::string error;
        ProtocolRpcJsonStatus status{ ProtocolRpcJsonStatus::InvalidSchema };
        if (!_decodeRoot(resultJson, root, status, error))
        {
            return _failure<SnapshotAndSubscribeResult>(status, std::move(error));
        }
        if (!_exact(root, { "subscriptionId", "generation", "cursor", "snapshot" }) ||
            !root["subscriptionId"].isString() || !_isUnsignedInteger(root["generation"]) ||
            !_isUnsignedInteger(root["cursor"]) || !root["snapshot"].isObject())
        {
            return _failure<SnapshotAndSubscribeResult>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "SnapshotAndSubscribe result fields are invalid");
        }

        SnapshotAndSubscribeResult value;
        value.SubscriptionId = root["subscriptionId"].asString();
        value.Generation = root["generation"].asUInt64();
        value.Cursor = root["cursor"].asUInt64();
        value.SnapshotJson = _write(root["snapshot"]);
        if (!IsCanonicalProtocolUuid(value.SubscriptionId) || value.Generation == 0)
        {
            return _failure<SnapshotAndSubscribeResult>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "SnapshotAndSubscribe identity is invalid");
        }
        ProtocolRpcJsonDecodeResult<SnapshotAndSubscribeResult> result;
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        result.Value = std::move(value);
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeSnapshotAndSubscribeResult(
        const SnapshotAndSubscribeResult& result)
    {
        ProtocolRpcJsonEncodeResult encoded;
        Json::Value snapshot;
        std::string error;
        if (!IsCanonicalProtocolUuid(result.SubscriptionId) || result.Generation == 0 ||
            result.SnapshotJson.size() > MaximumControlFrameBytes ||
            !IsValidUtf8(result.SnapshotJson) ||
            !_parse(result.SnapshotJson, snapshot, error) || !snapshot.isObject())
        {
            encoded.Error = "SnapshotAndSubscribe result is invalid";
            return encoded;
        }
        Json::Value root{ Json::objectValue };
        root["subscriptionId"] = result.SubscriptionId;
        root["generation"] = Json::UInt64{ result.Generation };
        root["cursor"] = Json::UInt64{ result.Cursor };
        root["snapshot"] = std::move(snapshot);
        return _encoded(root);
    }

    ProtocolRpcJsonDecodeResult<UnsubscribeRequest> DecodeUnsubscribeRequest(
        const std::string_view paramsJson)
    {
        Json::Value root;
        std::string error;
        ProtocolRpcJsonStatus status{ ProtocolRpcJsonStatus::InvalidSchema };
        if (!_decodeRoot(paramsJson, root, status, error))
        {
            return _failure<UnsubscribeRequest>(status, std::move(error));
        }
        if (!_exact(root, { "subscriptionId", "generation" }) ||
            !root["subscriptionId"].isString() || !_isUnsignedInteger(root["generation"]))
        {
            return _failure<UnsubscribeRequest>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "Unsubscribe request fields are invalid");
        }
        UnsubscribeRequest value;
        value.SubscriptionId = root["subscriptionId"].asString();
        value.Generation = root["generation"].asUInt64();
        if (!IsCanonicalProtocolUuid(value.SubscriptionId) || value.Generation == 0)
        {
            return _failure<UnsubscribeRequest>(
                ProtocolRpcJsonStatus::InvalidSchema,
                "Unsubscribe identity is invalid");
        }
        ProtocolRpcJsonDecodeResult<UnsubscribeRequest> result;
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        result.Value = std::move(value);
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeUnsubscribeRequest(const UnsubscribeRequest& request)
    {
        ProtocolRpcJsonEncodeResult result;
        if (!IsCanonicalProtocolUuid(request.SubscriptionId) || request.Generation == 0)
        {
            result.Error = "Unsubscribe request is invalid";
            return result;
        }
        Json::Value root{ Json::objectValue };
        root["subscriptionId"] = request.SubscriptionId;
        root["generation"] = Json::UInt64{ request.Generation };
        return _encoded(root);
    }
}
