// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "ITaskStore.h"
#include "ProjectCatalogRpc.h"
#include <filesystem>
#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    template<typename T> struct RuntimeWorkspaceResult
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
    class RuntimeProjectCatalogService final
    {
    public:
        RuntimeProjectCatalogService(std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store, std::filesystem::path runtimeRoot) :
            _store{ std::move(store) }, _runtimeRoot{ std::move(runtimeRoot) } {}
        bool Valid() const noexcept { return _store && _store->IsReady() && _runtimeRoot.is_absolute(); }
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::ProjectCatalogQueryResult> Query(const Microsoft::Terminal::TaskRuntimeProtocol::ProjectCatalogQueryRequest& request);
        RuntimeWorkspaceResult<Microsoft::Terminal::TaskRuntimeProtocol::ProjectCatalogCommandResult> Execute(const Microsoft::Terminal::TaskRuntimeProtocol::ProjectCatalogCommandRequest& request);
    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
        std::filesystem::path _runtimeRoot;
    };
}
