// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "RuntimeProductRouter.h"
#include "RuntimeProjectCatalogService.h"
#include "RuntimeTaskBoardService.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;
    namespace
    {
        RuntimeControlRouteResult failure(ProtocolRpcErrorCode code, std::string error)
        {
            RuntimeControlRouteResult result; result.Error = ProtocolRpcError{ code, std::move(error), "{}" }; return result;
        }
        ProtocolRpcErrorCode codeFor(TaskStoreCommandStatus status)
        {
            switch (status)
            {
            case TaskStoreCommandStatus::NotFound:
            case TaskStoreCommandStatus::Tombstoned: return ProtocolRpcErrorCode::NotFound;
            case TaskStoreCommandStatus::VersionConflict:
            case TaskStoreCommandStatus::IdempotencyConflict:
            case TaskStoreCommandStatus::InvalidTransition:
            case TaskStoreCommandStatus::ActiveRunExists:
            case TaskStoreCommandStatus::ActiveIntentExists: return ProtocolRpcErrorCode::Conflict;
            case TaskStoreCommandStatus::NotReady: return ProtocolRpcErrorCode::RuntimeUnavailable;
            case TaskStoreCommandStatus::InvalidCommand:
            case TaskStoreCommandStatus::InvalidRecord:
            case TaskStoreCommandStatus::CapacityExceeded: return ProtocolRpcErrorCode::InvalidRequest;
            default: return ProtocolRpcErrorCode::InternalError;
            }
        }
        template<typename Decode, typename Execute, typename Encode>
        RuntimeControlRouteResult dispatch(std::string_view payload, Decode decode, Execute execute, Encode encode)
        {
            const auto decoded = decode(payload);
            if (!decoded.Succeeded()) return failure(ProtocolRpcErrorCode::InvalidRequest, decoded.Error);
            const auto executed = execute(decoded.Value);
            if (!executed.Succeeded()) return failure(codeFor(executed.Status), executed.Error);
            const auto encoded = encode(executed.Value);
            if (!encoded.Succeeded) return failure(ProtocolRpcErrorCode::InternalError, encoded.Error);
            RuntimeControlRouteResult result; result.Succeeded = true; result.ResultJson = encoded.Payload; return result;
        }
    }
    RuntimeControlRouteResult RuntimeProductRouter::_RouteWorkspace(const RuntimeControlClientContext& client,
                                                                   const ControlRequest& request,
                                                                   const RuntimeControlCancellationToken& cancellation)
    {
        const auto required = RequiredCapabilities(request.Method);
        if (client.ClientKind != ProtocolClientKind::TerminalUi || (client.NegotiatedCapabilities & required) != required)
            return failure(ProtocolRpcErrorCode::CapabilityRequired, "Project and board operations require a supporting desktop client");
        const auto project = request.Method == ControlMethod::QueryProjects || request.Method == ControlMethod::ProjectCommand;
        if (cancellation.IsCancellationRequested() || (project ? (!_projects || !_projects->Valid()) : (!_board || !_board->Valid())))
            return failure(ProtocolRpcErrorCode::RuntimeUnavailable, "Project or board service is unavailable");
        switch (request.Method)
        {
        case ControlMethod::QueryProjects:
            return dispatch(request.PayloadJson, DecodeProjectCatalogQueryRequest,
                            [&](const auto& value) { return _projects->Query(value); }, EncodeProjectCatalogQueryResult);
        case ControlMethod::ProjectCommand:
            return dispatch(request.PayloadJson, DecodeProjectCatalogCommandRequest,
                            [&](const auto& value) { return _projects->Execute(value); }, EncodeProjectCatalogCommandResult);
        case ControlMethod::QueryTaskSummaries:
            return dispatch(request.PayloadJson, DecodeTaskSummaryQueryRequest,
                            [&](const auto& value) { return _board->Query(value); }, EncodeTaskSummaryQueryResult);
        case ControlMethod::TaskBoardCommand:
            return dispatch(request.PayloadJson, DecodeTaskBoardCommandRequest,
                            [&](const auto& value) { return _board->Execute(value); }, EncodeTaskBoardCommandResult);
        case ControlMethod::QueryBoardOperation:
            return dispatch(request.PayloadJson, DecodeTaskBoardOperationQueryRequest,
                            [&](const auto& value) { return _board->QueryOperation(value); }, EncodeTaskBoardCommandResult);
        case ControlMethod::QueryTaskSessionIndex:
            return dispatch(request.PayloadJson, DecodeTaskSessionIndexQueryRequest,
                            [&](const auto& value) { return _board->QuerySessions(value); }, EncodeTaskSessionIndexQueryResult);
        default: return failure(ProtocolRpcErrorCode::InvalidRequest, "Unknown project method");
        }
    }
}
