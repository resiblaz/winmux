// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProductRouter.h"

#include "RuntimeCrypto.h"

#include "RtaskCommandJson.h"

#include <json/json.h>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
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

        int64_t _unixMilliseconds() noexcept
        {
            FILETIME fileTime{};
            GetSystemTimeAsFileTime(&fileTime);
            ULARGE_INTEGER ticks{};
            ticks.LowPart = fileTime.dwLowDateTime;
            ticks.HighPart = fileTime.dwHighDateTime;
            constexpr uint64_t unixEpochInFileTimeTicks{ 116444736000000000ull };
            return ticks.QuadPart < unixEpochInFileTimeTicks ?
                       -1 :
                       static_cast<int64_t>(
                           (ticks.QuadPart - unixEpochInFileTimeTicks) / 10000);
        }

        ProtocolRpcErrorCode _rpcCode(const RuntimeRtaskCommandStatus status) noexcept
        {
            switch (status)
            {
            case RuntimeRtaskCommandStatus::InvalidRequest:
                return ProtocolRpcErrorCode::InvalidRequest;
            case RuntimeRtaskCommandStatus::Unauthorized:
                return ProtocolRpcErrorCode::CapabilityRequired;
            case RuntimeRtaskCommandStatus::NotFound:
                return ProtocolRpcErrorCode::NotFound;
            case RuntimeRtaskCommandStatus::Conflict:
                return ProtocolRpcErrorCode::Conflict;
            case RuntimeRtaskCommandStatus::StoreUnavailable:
                return ProtocolRpcErrorCode::RuntimeUnavailable;
            case RuntimeRtaskCommandStatus::ResourceFailure:
            case RuntimeRtaskCommandStatus::InvalidConfiguration:
            default:
                return ProtocolRpcErrorCode::InternalError;
            }
        }

        RuntimeControlRouteResult _routeRtask(
            RuntimeRtaskCommandService& service,
            const RuntimeControlClientContext& client,
            const ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation)
        {
            if (client.ClientKind != ProtocolClientKind::TaskCtl)
            {
                return _error(
                    ProtocolRpcErrorCode::CapabilityRequired,
                    "SubmitCommand requires an Agent-scoped TaskCtl client");
            }
            if (cancellation.IsCancellationRequested())
            {
                return _error(
                    ProtocolRpcErrorCode::RuntimeUnavailable,
                    "rtask command was cancelled before execution");
            }
            const auto decoded = DecodeRtaskCommandRequest(request.PayloadJson);
            if (!decoded.Succeeded())
            {
                return _error(
                    ProtocolRpcErrorCode::InvalidRequest,
                    "SubmitCommand rtask parameters are invalid");
            }
            const auto eventId = NewRuntimeUuid();
            const auto unixMilliseconds = _unixMilliseconds();
            if (!eventId || unixMilliseconds < 0)
            {
                return _error(
                    ProtocolRpcErrorCode::InternalError,
                    "Runtime could not create rtask command metadata");
            }

            const auto executed = service.Execute(
                decoded.Value,
                {
                    GetTickCount64(),
                    unixMilliseconds,
                    *eventId,
                });
            if (!executed.Succeeded())
            {
                return _error(
                    _rpcCode(executed.Status),
                    executed.Error.empty() ?
                        "rtask command was rejected" :
                        executed.Error);
            }
            // A cancellation observed after Execute cannot undo the durable
            // mutation. Return the correlated result so its outcome is known.
            auto encoded = EncodeRtaskCommandResult(executed.Result);
            if (!encoded.Succeeded)
            {
                return _error(
                    ProtocolRpcErrorCode::InternalError,
                    "Runtime could not encode the rtask command result");
            }
            RuntimeControlRouteResult result;
            result.Succeeded = true;
            result.ResultJson = std::move(encoded.Payload);
            return result;
        }
    }

    RuntimeProductRouter::RuntimeProductRouter(
        std::shared_ptr<RuntimeFoundationRouter> foundation,
        std::shared_ptr<RuntimeRtaskCommandService> rtask) noexcept :
        _foundation{ std::move(foundation) },
        _rtask{ std::move(rtask) }
    {
        _valid = _foundation && _foundation->Valid() && _rtask && _rtask->Valid();
    }

    bool RuntimeProductRouter::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeControlRouteResult RuntimeProductRouter::Route(
        const RuntimeControlClientContext& client,
        const ControlRequest& request,
        const RuntimeControlCancellationToken& cancellation)
    {
        try
        {
            if (!_valid)
            {
                return _error(
                    ProtocolRpcErrorCode::RuntimeUnavailable,
                    "Runtime product router is unavailable");
            }
            if (request.Method == ControlMethod::SubmitCommand)
            {
                return _routeRtask(*_rtask, client, request, cancellation);
            }
            return _foundation->Route(client, request, cancellation);
        }
        catch (...)
        {
            return _error(
                ProtocolRpcErrorCode::InternalError,
                "Runtime product routing failed");
        }
    }

    std::shared_ptr<RuntimeRtaskCommandService> RuntimeProductRouter::Rtask() const noexcept
    {
        return _rtask;
    }
}
