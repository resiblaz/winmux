// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeProductHost.h"
#include "RuntimeProductRouter.h"
#include "RuntimeTaskProviderService.h"
#include "RuntimeTaskTerminalService.h"
#include "RuntimeTaskOrchestrator.h"

#include "ITaskStore.h"

#include <condition_variable>
#include <atomic>
#include <mutex>
#include <thread>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeProductHost::Impl
    {
        mutable std::mutex LifecycleMutex;
        RuntimeBootstrapResult Bootstrap;
        std::string InstanceId;
        std::shared_ptr<RuntimeProductRouter> Router;
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> Store;
        std::shared_ptr<RuntimeTaskProviderService> Provider;
        std::shared_ptr<RuntimeTaskTerminalService> Terminals;
        std::shared_ptr<RuntimeTaskOrchestrator> Orchestrator;
        RuntimeControlServer Server;
        RuntimeProductHostIdleConfiguration IdleConfiguration;
        std::shared_ptr<std::atomic_bool> DrainRequested;

        std::mutex IdleMutex;
        std::condition_variable IdleCondition;
        bool IdleStopRequested{};
        std::thread IdleMonitor;

        Impl(
            RuntimeBootstrapResult bootstrap,
            std::string instanceId,
            std::shared_ptr<RuntimeProductRouter> router,
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
            std::shared_ptr<RuntimeTaskProviderService> provider,
            std::shared_ptr<RuntimeTaskTerminalService> terminals,
            std::shared_ptr<RuntimeTaskOrchestrator> orchestrator,
            std::shared_ptr<std::atomic_bool> drainRequested,
            RuntimeProductHostIdleConfiguration idleConfiguration);
        ~Impl();

        void Finalize() noexcept;
        [[nodiscard]] bool StartIdleMonitor() noexcept;
        void StopIdleMonitor() noexcept;
        void RunIdleMonitor() noexcept;
        [[nodiscard]] RuntimeHelperActivitySnapshot ObserveActivity() const noexcept;
    };
}
