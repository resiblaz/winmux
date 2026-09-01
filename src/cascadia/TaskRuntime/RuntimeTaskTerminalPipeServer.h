// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"
#include "RuntimeTaskTerminalBackend.h"
#include "RuntimeTerminalEndpoint.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeTaskTerminalPipeResult
    {
        bool Succeeded{};
        size_t ClientCount{};
        std::string Error;
    };

    class RuntimeTaskTerminalPipeServer final
    {
    public:
        RuntimeTaskTerminalPipeServer(
            RuntimeIdentity identity,
            std::string runtimeInstanceId,
            std::string taskId,
            RuntimeTerminalEndpoint endpoint,
            std::shared_ptr<RuntimeTaskTerminalBackend> backend);
        ~RuntimeTaskTerminalPipeServer();

        RuntimeTaskTerminalPipeServer(const RuntimeTaskTerminalPipeServer&) = delete;
        RuntimeTaskTerminalPipeServer& operator=(const RuntimeTaskTerminalPipeServer&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] bool ClaimSurface(std::string_view clientId) noexcept;
        [[nodiscard]] bool Start() noexcept;
        [[nodiscard]] RuntimeTaskTerminalPipeResult Publish(std::span<const uint8_t> output, uint32_t flags);
        [[nodiscard]] size_t ClientCount() const noexcept;
        [[nodiscard]] const RuntimeTerminalEndpoint& Endpoint() const noexcept;
        void Stop(bool graceful) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
