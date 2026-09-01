// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeTaskTerminalBackend.h"

#include <memory>
#include <mutex>
#include <span>
#include <string_view>

namespace Microsoft::Terminal::TaskCore
{
    class ITaskStore;
}

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeTaskOrchestrator;
    class RuntimeTaskProviderService;

    class RuntimeTaskTerminalCallbackBridge final
    {
    public:
        explicit RuntimeTaskTerminalCallbackBridge(
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store);

        void SetProvider(const std::shared_ptr<RuntimeTaskProviderService>& provider) noexcept;
        void SetOrchestrator(const std::shared_ptr<RuntimeTaskOrchestrator>& orchestrator) noexcept;

        [[nodiscard]] RuntimeTerminalBackendResult Input(
            std::string_view taskId,
            std::span<const uint8_t> input);
        [[nodiscard]] RuntimeTerminalBackendResult Interrupt(std::string_view taskId);

    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
        std::weak_ptr<RuntimeTaskProviderService> _provider;
        std::weak_ptr<RuntimeTaskOrchestrator> _orchestrator;
        std::mutex _mutex;
    };
}
