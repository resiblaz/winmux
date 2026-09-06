// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskStore.h"
#include "TaskProjectionRpc.h"
#include "RuntimeTaskProviderService.h"
#include "RuntimeTaskTerminalService.h"

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeTaskOrchestrator;
    enum class RuntimeTaskProjectionStatus
    {
        Succeeded,
        Replayed,
        InvalidRequest,
        NotFound,
        Conflict,
        StoreUnavailable,
        ResourceFailure,
    };

    struct RuntimeTaskProjectionQueryResult
    {
        RuntimeTaskProjectionStatus Status{ RuntimeTaskProjectionStatus::ResourceFailure };
        Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionQueryResult Projection;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeTaskProjectionStatus::Succeeded;
        }
    };

    struct RuntimeTaskProjectionCommandResult
    {
        RuntimeTaskProjectionStatus Status{ RuntimeTaskProjectionStatus::ResourceFailure };
        Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandResult Result;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeTaskProjectionStatus::Succeeded ||
                   Status == RuntimeTaskProjectionStatus::Replayed;
        }
    };

    class RuntimeTaskProjectionService final
    {
    public:
        RuntimeTaskProjectionService(
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
            std::shared_ptr<RuntimeTaskProviderService> provider,
            std::filesystem::path runtimeDataRoot,
            std::shared_ptr<RuntimeTaskTerminalService> terminals = nullptr,
            std::shared_ptr<RuntimeTaskOrchestrator> orchestrator = nullptr) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeTaskProjectionQueryResult Query(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionQueryRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult Execute(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request,
            HANDLE cancellationEvent,
            std::string_view clientInstanceId = {});

    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
        std::shared_ptr<RuntimeTaskProviderService> _provider;
        std::shared_ptr<RuntimeTaskTerminalService> _terminals;
        std::shared_ptr<RuntimeTaskOrchestrator> _orchestrator;
        std::filesystem::path _runtimeDataRoot;
        std::mutex _mutex;
        uint64_t _generation{ 1 };

        [[nodiscard]] RuntimeTaskProjectionCommandResult _create(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _updateAgent(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _start(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _stop(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request,
            HANDLE cancellationEvent);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _open(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request,
            std::string_view clientInstanceId);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _close(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _status(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _message(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _retry(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _intent(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _derive(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request);
        [[nodiscard]] RuntimeTaskProjectionCommandResult _restartProvider(
            const Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& request,
            HANDLE cancellationEvent);
    };
}
