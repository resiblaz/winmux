// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "ITaskStore.h"
#include "AgentCatalogRpc.h"
#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    template<typename T>
    struct RuntimeAgentCatalogResult
    {
        Microsoft::Terminal::TaskCore::TaskStoreCommandStatus Status{ Microsoft::Terminal::TaskCore::TaskStoreCommandStatus::StorageError };
        T Value;
        std::string Error;
        bool Succeeded() const noexcept
        {
            using Microsoft::Terminal::TaskCore::TaskStoreCommandStatus;
            return Status == TaskStoreCommandStatus::Succeeded || Status == TaskStoreCommandStatus::Replayed;
        }
    };

    class RuntimeAgentCatalogService final
    {
    public:
        explicit RuntimeAgentCatalogService(std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store) : _store{ std::move(store) } {}
        bool Valid() const noexcept { return _store && _store->IsReady(); }
        RuntimeAgentCatalogResult<Microsoft::Terminal::TaskRuntimeProtocol::AgentCatalogQueryResult> Query(const Microsoft::Terminal::TaskRuntimeProtocol::AgentCatalogQueryRequest& request);
        RuntimeAgentCatalogResult<Microsoft::Terminal::TaskRuntimeProtocol::AgentCatalogCommandResult> Execute(const Microsoft::Terminal::TaskRuntimeProtocol::AgentCatalogCommandRequest& request);
    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
    };
}
