// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "ProjectCatalogRpc.h"
#include "TaskBoardRpc.h"
#include <functional>
#include <memory>

namespace winrt::TerminalApp::implementation::projects
{
    namespace protocol = ::Microsoft::Terminal::TaskRuntimeProtocol;
    template<typename T> struct WorkspaceReply
    {
        bool Succeeded{};
        bool OutcomeUnknown{};
        protocol::ProtocolRpcErrorCode ErrorCode{ protocol::ProtocolRpcErrorCode::InternalError };
        std::string Error;
        T Value;
    };
    class IWorkspaceSource
    {
    public:
        virtual ~IWorkspaceSource() = default;
        virtual bool SupportsProjects() const noexcept = 0;
        virtual bool SupportsBoard() const noexcept = 0;
        virtual std::string Epoch() const = 0;
        virtual void QueryProjects(protocol::ProjectCatalogQueryRequest request, std::function<void(WorkspaceReply<protocol::ProjectCatalogQueryResult>)> completed) = 0;
        virtual void ExecuteProject(protocol::ProjectCatalogCommandRequest request, std::function<void(WorkspaceReply<protocol::ProjectCatalogCommandResult>)> completed) = 0;
        virtual void QuerySummaries(protocol::TaskSummaryQueryRequest request, std::function<void(WorkspaceReply<protocol::TaskSummaryQueryResult>)> completed) = 0;
        virtual void ExecuteBoard(protocol::TaskBoardCommandRequest request, std::function<void(WorkspaceReply<protocol::TaskBoardCommandResult>)> completed) = 0;
        virtual void QueryOperation(protocol::TaskBoardOperationQueryRequest request, std::function<void(WorkspaceReply<protocol::TaskBoardCommandResult>)> completed) = 0;
        virtual bool Connected() const noexcept { return true; }
    };
}
