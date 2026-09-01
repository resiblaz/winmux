// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskStore.h"
#include "RuntimeRtaskCommandService.h"
#include "RuntimeTaskProviderService.h"
#include "RuntimeTaskTerminalService.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeTaskOrchestratorConfiguration
    {
        size_t MaximumConcurrentPreparations{ 4 };
        uint32_t ActivePollMilliseconds{ 500 };
        uint32_t IdlePollMilliseconds{ 5000 };
        uint32_t TerminalIdleMilliseconds{ 5 * 60 * 1000 };

        [[nodiscard]] bool Valid() const noexcept;
    };

    class RuntimeTaskOrchestrator final
    {
    public:
        RuntimeTaskOrchestrator(
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
            std::shared_ptr<RuntimeTaskProviderService> provider,
            std::shared_ptr<RuntimeRtaskCommandService> rtask,
            std::shared_ptr<RuntimeTaskTerminalService> terminals,
            std::filesystem::path runtimeDataRoot,
            std::filesystem::path gitExecutable,
            std::filesystem::path rtaskExecutable,
            std::string rtaskEndpoint,
            RuntimeTaskOrchestratorConfiguration configuration = {});
        ~RuntimeTaskOrchestrator();

        RuntimeTaskOrchestrator(const RuntimeTaskOrchestrator&) = delete;
        RuntimeTaskOrchestrator& operator=(const RuntimeTaskOrchestrator&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] bool Start();
        void Notify() noexcept;
        void Stop() noexcept;
        [[nodiscard]] bool Running() const noexcept;
        [[nodiscard]] bool SubmitTerminalMessage(
            std::string_view taskId,
            std::span<const uint8_t> message) noexcept;
        [[nodiscard]] bool RequestStop(std::string_view taskId) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
