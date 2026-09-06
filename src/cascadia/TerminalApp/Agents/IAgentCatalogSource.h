// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "AgentCatalogRpc.h"
#include "TaskProjectionRpc.h"
#include <functional>
#include <memory>

namespace winrt::TerminalApp::implementation::agents
{
    namespace protocol = ::Microsoft::Terminal::TaskRuntimeProtocol;
    template<typename T>
    struct CatalogReply
    {
        bool Succeeded{};
        bool OutcomeUnknown{};
        protocol::ProtocolRpcErrorCode ErrorCode{ protocol::ProtocolRpcErrorCode::InternalError };
        std::string Error;
        T Value;
    };

    class IAgentCatalogSource
    {
    public:
        virtual ~IAgentCatalogSource() = default;
        virtual void Query(protocol::AgentCatalogQueryRequest request, std::function<void(CatalogReply<protocol::AgentCatalogQueryResult>)> completed) = 0;
        virtual void Execute(protocol::AgentCatalogCommandRequest request, std::function<void(CatalogReply<protocol::AgentCatalogCommandResult>)> completed) = 0;
        virtual void ExecuteTask(protocol::TaskUiCommandRequest request, std::function<void(CatalogReply<protocol::TaskUiCommandResult>)> completed) = 0;
        virtual void QueryTask(protocol::TaskProjectionQueryRequest request, std::function<void(CatalogReply<protocol::TaskProjectionQueryResult>)> completed) = 0;
    };
}
