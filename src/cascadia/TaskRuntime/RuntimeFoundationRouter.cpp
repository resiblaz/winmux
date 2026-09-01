// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeFoundationRouter.h"

#include "RuntimeFoundationRpc.h"
#include "ProtocolValidation.h"

#include <json/json.h>

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        bool _parseEmptyObject(const std::string_view payload)
        {
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false;
            builder["allowTrailingCommas"] = false;
            builder["collectComments"] = false;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            builder["strictRoot"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            Json::Value value;
            std::string error;
            return reader &&
                   reader->parse(payload.data(), payload.data() + payload.size(), &value, &error) &&
                   value.isObject() && value.size() == 0;
        }

        std::string _writeJson(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            builder["indentation"] = "";
            return Json::writeString(builder, value);
        }

        RuntimeControlRouteResult _error(
            const ProtocolRpcErrorCode code,
            std::string message,
            std::string dataJson = "{}")
        {
            RuntimeControlRouteResult result;
            result.Error = ProtocolRpcError{
                code,
                std::move(message),
                std::move(dataJson),
            };
            return result;
        }

        RuntimeControlRouteResult _invalidParameters(const ControlMethod method)
        {
            Json::Value data{ Json::objectValue };
            data["method"] = std::string{ ControlMethodName(method) };
            data["requirement"] = "params_must_be_empty_object";
            return _error(
                ProtocolRpcErrorCode::InvalidRequest,
                "Runtime foundation request parameters must be an empty object",
                _writeJson(data));
        }

        RuntimeControlRouteResult _encodedResult(ProtocolRpcJsonEncodeResult encoded)
        {
            if (!encoded.Succeeded)
            {
                return _error(
                    ProtocolRpcErrorCode::InternalError,
                    "Runtime foundation result encoding failed");
            }
            RuntimeControlRouteResult result;
            result.Succeeded = true;
            result.ResultJson = std::move(encoded.Payload);
            return result;
        }
    }

    RuntimeFoundationRouter::RuntimeFoundationRouter(
        std::string runtimeIdentity,
        std::string runtimeInstanceId,
        const uint32_t processId,
        std::function<bool()> requestDrain) :
        _runtimeIdentity{ std::move(runtimeIdentity) },
        _runtimeInstanceId{ std::move(runtimeInstanceId) },
        _processId{ processId },
        _requestDrain{ std::move(requestDrain) }
    {
    }

    bool RuntimeFoundationRouter::Valid() const noexcept
    {
        return IsValidRuntimeIdentity(_runtimeIdentity) &&
               IsCanonicalProtocolUuid(_runtimeInstanceId) &&
               _processId != 0;
    }

    RuntimeControlRouteResult RuntimeFoundationRouter::Route(
        const RuntimeControlClientContext& client,
        const ControlRequest& request,
        const RuntimeControlCancellationToken& cancellation)
    {
        if (cancellation.IsCancellationRequested())
        {
            return _error(
                ProtocolRpcErrorCode::RuntimeUnavailable,
                "Runtime foundation request was cancelled",
                "{\"state\":\"stopping\"}");
        }
        if (!Valid())
        {
            return _error(
                ProtocolRpcErrorCode::RuntimeUnavailable,
                "Runtime foundation is unavailable");
        }

        switch (request.Method)
        {
        case ControlMethod::Ping:
        {
            if (!_parseEmptyObject(request.PayloadJson))
            {
                return _invalidParameters(request.Method);
            }
            return _encodedResult(EncodePingResult({
                _runtimeInstanceId,
                client.ClientInstanceId,
            }));
        }
        case ControlMethod::RuntimeStatus:
        {
            if (!_parseEmptyObject(request.PayloadJson))
            {
                return _invalidParameters(request.Method);
            }
            return _encodedResult(EncodeRuntimeStatusResult({
                _runtimeIdentity,
                _runtimeInstanceId,
                _processId,
                std::string{ RuntimeFoundationReadyState },
            }));
        }
        case ControlMethod::DrainRuntime:
        {
            if (!_parseEmptyObject(request.PayloadJson))
            {
                return _invalidParameters(request.Method);
            }
            if (!_requestDrain || !_requestDrain())
            {
                return _error(ProtocolRpcErrorCode::Conflict, "Runtime drain request was rejected");
            }
            RuntimeControlRouteResult result;
            result.Succeeded = true;
            result.ResultJson = R"({"draining":true})";
            return result;
        }
        default:
        {
            Json::Value data{ Json::objectValue };
            const auto methodName = ControlMethodName(request.Method);
            if (!methodName.empty())
            {
                data["method"] = std::string{ methodName };
            }
            return _error(
                ProtocolRpcErrorCode::InvalidRequest,
                "Control method is not supported by the Runtime foundation",
                _writeJson(data));
        }
        }
    }
}
