// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "RuntimeProjectCatalogService.h"
#include "TaskBoardRpc.h"
#include "TaskSessionIndexRpc.h"

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeTaskOrchestrator;
    class RuntimeTaskBoardService final
    {
    public:
        RuntimeTaskBoardService(std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store, std::shared_ptr<RuntimeTaskOrchestrator> orchestrator = nullptr) :
            _store{ std::move(store) }, _orchestrator{ std::move(orchestrator) } {}
        bool Valid() const noexcept { return _store && _store->IsReady(); }
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskSummaryQueryResult> Query(const Microsoft::Terminal::TaskRuntimeProtocol::TaskSummaryQueryRequest& request);
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardCommandResult> Execute(const Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardCommandRequest& request);
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardCommandResult> QueryOperation(const Microsoft::Terminal::TaskRuntimeProtocol::TaskBoardOperationQueryRequest& request);
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::TaskSessionIndexQueryResult> QuerySessions(const Microsoft::Terminal::TaskRuntimeProtocol::TaskSessionIndexQueryRequest& request);
    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
        std::shared_ptr<RuntimeTaskOrchestrator> _orchestrator;
    };
}
