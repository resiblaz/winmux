// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ProtocolRpcJson.h"

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

        bool _exact(const Json::Value& value, const std::initializer_list<std::string_view> names)
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

        bool _parseBody(const std::string_view payload, Json::Value& body, std::string& error)
        {
            return payload.size() <= MaximumControlFrameBytes && IsValidUtf8(payload) &&
                   _parse(payload, body, error) && body.isObject();
        }

        bool _decodeRoot(const std::string_view payload, Json::Value& root, ProtocolRpcJsonStatus& status, std::string& error)
        {
            if (payload.size() > MaximumControlFrameBytes)
            {
                status = ProtocolRpcJsonStatus::Oversized;
                error = "RPC JSON exceeds the control payload limit";
                return false;
            }
            if (!IsValidUtf8(payload))
            {
                status = ProtocolRpcJsonStatus::InvalidUtf8;
                error = "RPC JSON is not valid UTF-8";
                return false;
            }
            if (!_parse(payload, root, error))
            {
                status = ProtocolRpcJsonStatus::MalformedJson;
                return false;
            }
            return true;
        }

        ProtocolRpcJsonEncodeResult _encoded(Json::Value root)
        {
            ProtocolRpcJsonEncodeResult result;
            result.Payload = _write(root);
            if (result.Payload.empty() || result.Payload.size() > MaximumControlFrameBytes ||
                !IsValidUtf8(result.Payload))
            {
                result.Payload.clear();
                result.Error = "Encoded RPC JSON exceeds protocol bounds";
                return result;
            }
            result.Succeeded = true;
            return result;
        }

        bool _validError(const ProtocolRpcError& error)
        {
            return !ProtocolRpcErrorCodeName(error.Code).empty() &&
                   !error.Message.empty() && error.Message.size() <= MaximumRpcErrorMessageBytes &&
                   IsValidUtf8(error.Message);
        }
    }

    ProtocolRpcJsonDecodeResult<ControlRequest> DecodeControlRequest(const std::string_view payload)
    {
        Json::Value root;
        std::string error;
        ProtocolRpcJsonStatus status{ ProtocolRpcJsonStatus::InvalidSchema };
        if (!_decodeRoot(payload, root, status, error))
        {
            return _failure<ControlRequest>(status, std::move(error));
        }
        if (!_exact(root, { "type", "requestId", "method", "params" }) ||
            !root["type"].isString() || root["type"].asString() != "request" ||
            !root["requestId"].isString() || !root["method"].isString() ||
            !root["params"].isObject())
        {
            return _failure<ControlRequest>(ProtocolRpcJsonStatus::InvalidSchema, "Control request fields are invalid");
        }
        const auto method = ControlMethodFromName(root["method"].asString());
        ControlRequest request;
        request.RequestId = root["requestId"].asString();
        if (!method || !IsCanonicalProtocolUuid(request.RequestId))
        {
            return _failure<ControlRequest>(ProtocolRpcJsonStatus::InvalidSchema, "Control request identity or method is invalid");
        }
        request.Method = method.value();
        request.PayloadJson = _write(root["params"]);
        ProtocolRpcJsonDecodeResult<ControlRequest> result;
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        result.Value = std::move(request);
        return result;
    }

    ProtocolRpcJsonDecodeResult<ControlResponse> DecodeControlResponse(const std::string_view payload)
    {
        Json::Value root;
        std::string error;
        ProtocolRpcJsonStatus status{ ProtocolRpcJsonStatus::InvalidSchema };
        if (!_decodeRoot(payload, root, status, error))
        {
            return _failure<ControlResponse>(status, std::move(error));
        }
        if (!root.isObject() || !root["type"].isString() || root["type"].asString() != "response" ||
            !root["requestId"].isString() || !root["ok"].isBool())
        {
            return _failure<ControlResponse>(ProtocolRpcJsonStatus::InvalidSchema, "Control response fields are invalid");
        }

        ControlResponse response;
        response.RequestId = root["requestId"].asString();
        response.Succeeded = root["ok"].asBool();
        if (!IsCanonicalProtocolUuid(response.RequestId))
        {
            return _failure<ControlResponse>(ProtocolRpcJsonStatus::InvalidSchema, "Control response request ID is invalid");
        }
        if (response.Succeeded)
        {
            if (!_exact(root, { "type", "requestId", "ok", "result" }) || !root["result"].isObject())
            {
                return _failure<ControlResponse>(ProtocolRpcJsonStatus::InvalidSchema, "Successful response result is invalid");
            }
            response.ResultJson = _write(root["result"]);
        }
        else
        {
            if (!_exact(root, { "type", "requestId", "ok", "error" }) ||
                !_exact(root["error"], { "code", "message", "data" }) ||
                !root["error"]["code"].isString() || !root["error"]["message"].isString() ||
                !root["error"]["data"].isObject())
            {
                return _failure<ControlResponse>(ProtocolRpcJsonStatus::InvalidSchema, "Error response payload is invalid");
            }
            const auto code = ProtocolRpcErrorCodeFromName(root["error"]["code"].asString());
            ProtocolRpcError rpcError;
            rpcError.Code = code.value_or(ProtocolRpcErrorCode::InternalError);
            rpcError.Message = root["error"]["message"].asString();
            rpcError.DataJson = _write(root["error"]["data"]);
            if (!code || !_validError(rpcError))
            {
                return _failure<ControlResponse>(ProtocolRpcJsonStatus::InvalidSchema, "Error response code or message is invalid");
            }
            response.ResultJson = "{}";
            response.Error = std::move(rpcError);
        }

        ProtocolRpcJsonDecodeResult<ControlResponse> result;
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        result.Value = std::move(response);
        return result;
    }

    ProtocolRpcJsonDecodeResult<ControlNotification> DecodeControlNotification(const std::string_view payload)
    {
        Json::Value root;
        std::string error;
        ProtocolRpcJsonStatus status{ ProtocolRpcJsonStatus::InvalidSchema };
        if (!_decodeRoot(payload, root, status, error))
        {
            return _failure<ControlNotification>(status, std::move(error));
        }
        if (!_exact(root, { "type", "subscriptionId", "generation", "sequence", "event", "payload" }) ||
            !root["type"].isString() || root["type"].asString() != "notification" ||
            !root["subscriptionId"].isString() || !root["generation"].isUInt64() ||
            !root["sequence"].isUInt64() || !root["event"].isString() || !root["payload"].isObject())
        {
            return _failure<ControlNotification>(ProtocolRpcJsonStatus::InvalidSchema, "Control notification fields are invalid");
        }

        ControlNotification notification;
        notification.SubscriptionId = root["subscriptionId"].asString();
        notification.Generation = root["generation"].asUInt64();
        notification.Sequence = root["sequence"].asUInt64();
        const auto eventName = root["event"].asString();
        const auto kind = ControlNotificationKindFromName(eventName);
        if (!IsCanonicalProtocolUuid(notification.SubscriptionId) ||
            notification.Generation == 0 || notification.Sequence == 0 ||
            eventName.empty() || eventName.size() > MaximumNotificationKindBytes)
        {
            return _failure<ControlNotification>(ProtocolRpcJsonStatus::InvalidSchema, "Control notification identity or cursor is invalid");
        }
        if (!kind)
        {
            notification.UnknownKind = eventName;
            notification.PayloadJson = _write(root["payload"]);
            ProtocolRpcJsonDecodeResult<ControlNotification> result;
            result.Status = ProtocolRpcJsonStatus::UnknownNotification;
            result.Value = std::move(notification);
            result.Error = "Control notification kind is unknown";
            return result;
        }
        notification.Kind = kind.value();
        notification.PayloadJson = _write(root["payload"]);
        ProtocolRpcJsonDecodeResult<ControlNotification> result;
        result.Status = ProtocolRpcJsonStatus::Succeeded;
        result.Value = std::move(notification);
        return result;
    }

    ProtocolRpcJsonEncodeResult EncodeControlRequest(const ControlRequest& request)
    {
        ProtocolRpcJsonEncodeResult result;
        Json::Value body;
        std::string error;
        if (!IsCanonicalProtocolUuid(request.RequestId) || ControlMethodName(request.Method).empty() ||
            !_parseBody(request.PayloadJson, body, error))
        {
            result.Error = "Control request is invalid";
            return result;
        }
        Json::Value root{ Json::objectValue };
        root["type"] = "request";
        root["requestId"] = request.RequestId;
        root["method"] = std::string{ ControlMethodName(request.Method) };
        root["params"] = std::move(body);
        return _encoded(std::move(root));
    }

    ProtocolRpcJsonEncodeResult EncodeControlResponse(const ControlResponse& response)
    {
        ProtocolRpcJsonEncodeResult result;
        if (!IsCanonicalProtocolUuid(response.RequestId) || response.Succeeded == response.Error.has_value())
        {
            result.Error = "Control response state is invalid";
            return result;
        }
        Json::Value root{ Json::objectValue };
        root["type"] = "response";
        root["requestId"] = response.RequestId;
        root["ok"] = response.Succeeded;
        Json::Value body;
        std::string error;
        if (response.Succeeded)
        {
            if (!_parseBody(response.ResultJson, body, error))
            {
                result.Error = "Control response result is invalid";
                return result;
            }
            root["result"] = std::move(body);
        }
        else
        {
            if (!_validError(response.Error.value()) ||
                !_parseBody(response.Error->DataJson, body, error))
            {
                result.Error = "Control response error is invalid";
                return result;
            }
            root["error"]["code"] = std::string{ ProtocolRpcErrorCodeName(response.Error->Code) };
            root["error"]["message"] = response.Error->Message;
            root["error"]["data"] = std::move(body);
        }
        return _encoded(std::move(root));
    }

    ProtocolRpcJsonEncodeResult EncodeControlNotification(const ControlNotification& notification)
    {
        ProtocolRpcJsonEncodeResult result;
        Json::Value body;
        std::string error;
        if (!IsCanonicalProtocolUuid(notification.SubscriptionId) ||
            notification.Generation == 0 || notification.Sequence == 0 ||
            !notification.UnknownKind.empty() ||
            ControlNotificationKindName(notification.Kind).empty() ||
            !_parseBody(notification.PayloadJson, body, error))
        {
            result.Error = "Control notification is invalid";
            return result;
        }
        Json::Value root{ Json::objectValue };
        root["type"] = "notification";
        root["subscriptionId"] = notification.SubscriptionId;
        root["generation"] = Json::UInt64{ notification.Generation };
        root["sequence"] = Json::UInt64{ notification.Sequence };
        root["event"] = std::string{ ControlNotificationKindName(notification.Kind) };
        root["payload"] = std::move(body);
        return _encoded(std::move(root));
    }
}
