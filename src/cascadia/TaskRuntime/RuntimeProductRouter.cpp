// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProductRouter.h"
#include "RuntimeAgentCatalogService.h"

#include "RuntimeCrypto.h"

#include "RtaskCommandJson.h"
#include "TaskProjectionRpc.h"

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

        ProtocolRpcErrorCode _taskRpcCode(const RuntimeTaskProjectionStatus status) noexcept
        {
            switch (status)
            {
            case RuntimeTaskProjectionStatus::InvalidRequest: return ProtocolRpcErrorCode::InvalidRequest;
            case RuntimeTaskProjectionStatus::NotFound: return ProtocolRpcErrorCode::NotFound;
            case RuntimeTaskProjectionStatus::Conflict: return ProtocolRpcErrorCode::Conflict;
            case RuntimeTaskProjectionStatus::StoreUnavailable: return ProtocolRpcErrorCode::RuntimeUnavailable;
            default: return ProtocolRpcErrorCode::InternalError;
            }
        }

        RuntimeControlRouteResult _routeTaskQuery(
            RuntimeTaskProjectionService& service,
            const RuntimeControlClientContext& client,
            const ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation)
        {
            if (client.ClientKind == ProtocolClientKind::TaskCtl)
            {
                return _error(ProtocolRpcErrorCode::CapabilityRequired, "TaskCtl cannot query UI Task projections");
            }
            if (cancellation.IsCancellationRequested())
            {
                return _error(ProtocolRpcErrorCode::RuntimeUnavailable, "Task projection query was cancelled");
            }
            const auto decoded = DecodeTaskProjectionQueryRequest(request.PayloadJson);
            if (!decoded.Succeeded()) return _error(ProtocolRpcErrorCode::InvalidRequest, "Task projection query is invalid");
            const auto queried = service.Query(decoded.Value);
            if (!queried.Succeeded()) return _error(_taskRpcCode(queried.Status), queried.Error);
            auto projection = queried.Projection;
            const auto agentAware = HasCapability(client.NegotiatedCapabilities, ProtocolCapability::AgentProfiles);
            auto encoded = EncodeTaskProjectionQueryResult(projection, agentAware);
            while (!encoded.Succeeded && encoded.Error == "Encoded Task projection exceeds protocol bounds" &&
                   !decoded.Value.TaskId && projection.Tasks.size() > 1)
            {
                projection.Tasks.resize((projection.Tasks.size() + 1) / 2);
                encoded = EncodeTaskProjectionQueryResult(projection, agentAware);
            }
            if (!encoded.Succeeded) return _error(ProtocolRpcErrorCode::InternalError, "Task projection could not be encoded");
            RuntimeControlRouteResult result;
            result.Succeeded = true;
            result.ResultJson = encoded.Payload;
            return result;
        }

        RuntimeControlRouteResult _routeTaskCommand(
            RuntimeTaskProjectionService& service,
            const RuntimeControlClientContext& client,
            const ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation)
        {
            if (client.ClientKind == ProtocolClientKind::TaskCtl)
            {
                return _error(ProtocolRpcErrorCode::CapabilityRequired, "TaskCtl cannot issue UI Task commands");
            }
            const auto decoded = DecodeTaskUiCommandRequest(request.PayloadJson);
            if (!decoded.Succeeded()) return _error(ProtocolRpcErrorCode::InvalidRequest, "Task UI command is invalid");
            if ((decoded.Value.Operation == TaskUiCommandOperation::UpdateAgent || (decoded.Value.Draft && decoded.Value.Draft->Agent)) &&
                !HasCapability(client.NegotiatedCapabilities, ProtocolCapability::AgentProfiles))
                return _error(ProtocolRpcErrorCode::CapabilityRequired, "Task Agent configuration requires Agent Profiles support");
            if (cancellation.IsCancellationRequested()) return _error(ProtocolRpcErrorCode::RuntimeUnavailable, "Task UI command was cancelled");
            const auto executed = service.Execute(decoded.Value, nullptr, client.ClientInstanceId);
            if (!executed.Succeeded()) return _error(_taskRpcCode(executed.Status), executed.Error);
            const auto encoded = EncodeTaskUiCommandResult(executed.Result);
            if (!encoded.Succeeded) return _error(ProtocolRpcErrorCode::InternalError, "Task command result could not be encoded");
            RuntimeControlRouteResult result;
            result.Succeeded = true;
            result.ResultJson = encoded.Payload;
            return result;
        }
    }

    RuntimeProductRouter::RuntimeProductRouter(
        std::shared_ptr<RuntimeFoundationRouter> foundation,
        std::shared_ptr<RuntimeRtaskCommandService> rtask,
        std::shared_ptr<RuntimeTaskProviderService> provider,
        std::shared_ptr<RuntimeTaskProjectionService> tasks,
        std::shared_ptr<RuntimeAgentCatalogService> agents) noexcept :
        _foundation{ std::move(foundation) },
        _rtask{ std::move(rtask) },
        _provider{ std::move(provider) },
        _tasks{ std::move(tasks) },
        _agents{ std::move(agents) }
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
                if (client.ClientKind == ProtocolClientKind::TaskCtl)
                {
                    return _routeRtask(*_rtask, client, request, cancellation);
                }
                if (DecodeRtaskCommandRequest(request.PayloadJson).Succeeded())
                {
                    return _error(ProtocolRpcErrorCode::CapabilityRequired, "Agent-scoped rtask commands require TaskCtl");
                }
                return _tasks ? _routeTaskCommand(*_tasks, client, request, cancellation) :
                                _error(ProtocolRpcErrorCode::RuntimeUnavailable, "Runtime Task command service is unavailable");
            }
            if (request.Method == ControlMethod::QueryAgents || request.Method == ControlMethod::AgentCommand)
            {
                if (client.ClientKind != ProtocolClientKind::TerminalUi || !HasCapability(client.NegotiatedCapabilities, ProtocolCapability::AgentProfiles))
                    return _error(ProtocolRpcErrorCode::CapabilityRequired, "Agent catalog requires an Agent-aware desktop client");
                if (!_agents || !_agents->Valid() || cancellation.IsCancellationRequested())
                    return _error(ProtocolRpcErrorCode::RuntimeUnavailable, "Agent catalog is unavailable");
                const auto failure = [&](const auto& result) {
                    using Microsoft::Terminal::TaskCore::TaskStoreCommandStatus;
                    const auto code = result.Status == TaskStoreCommandStatus::NotFound ? ProtocolRpcErrorCode::NotFound :
                                      result.Status == TaskStoreCommandStatus::VersionConflict || result.Status == TaskStoreCommandStatus::IdempotencyConflict ? ProtocolRpcErrorCode::Conflict :
                                      result.Status == TaskStoreCommandStatus::NotReady ? ProtocolRpcErrorCode::RuntimeUnavailable :
                                      result.Status == TaskStoreCommandStatus::StorageError ? ProtocolRpcErrorCode::InternalError : ProtocolRpcErrorCode::InvalidRequest;
                    return _error(code, result.Error);
                };
                ProtocolRpcJsonEncodeResult encoded;
                if (request.Method == ControlMethod::QueryAgents)
                {
                    const auto decoded = DecodeAgentCatalogQueryRequest(request.PayloadJson);
                    if (!decoded.Succeeded()) return _error(ProtocolRpcErrorCode::InvalidRequest, decoded.Error);
                    const auto queried = _agents->Query(decoded.Value);
                    if (!queried.Succeeded()) return failure(queried);
                    encoded = EncodeAgentCatalogQueryResult(queried.Value);
                }
                else
                {
                    const auto decoded = DecodeAgentCatalogCommandRequest(request.PayloadJson);
                    if (!decoded.Succeeded()) return _error(ProtocolRpcErrorCode::InvalidRequest, decoded.Error);
                    const auto executed = _agents->Execute(decoded.Value);
                    if (!executed.Succeeded()) return failure(executed);
                    encoded = EncodeAgentCatalogCommandResult(executed.Value);
                }
                if (!encoded.Succeeded) return _error(ProtocolRpcErrorCode::InternalError, encoded.Error);
                RuntimeControlRouteResult result;
                result.Succeeded = true; result.ResultJson = std::move(encoded.Payload);
                return result;
            }
            if (request.Method == ControlMethod::QueryTask)
            {
                return _tasks ? _routeTaskQuery(*_tasks, client, request, cancellation) :
                                _error(ProtocolRpcErrorCode::RuntimeUnavailable, "Runtime Task projection service is unavailable");
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

    std::shared_ptr<RuntimeTaskProviderService> RuntimeProductRouter::Provider() const noexcept
    {
        return _provider;
    }

    std::shared_ptr<RuntimeTaskProjectionService> RuntimeProductRouter::Tasks() const noexcept
    {
        return _tasks;
    }
}
