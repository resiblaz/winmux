// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeTerminalBackendStatus
    {
        Succeeded,
        Rejected,
        Closed,
        SystemError,
    };

    struct RuntimeTerminalBackendResult
    {
        RuntimeTerminalBackendStatus Status{
            RuntimeTerminalBackendStatus::SystemError
        };
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeTerminalBackendStatus::Succeeded;
        }
    };

    struct RuntimeTerminalBackendSnapshotResult : RuntimeTerminalBackendResult
    {
        std::vector<uint8_t> Payload;
    };

    class IRuntimeTerminalBackend
    {
    public:
        virtual ~IRuntimeTerminalBackend() = default;
        [[nodiscard]] virtual RuntimeTerminalBackendSnapshotResult Snapshot() = 0;
        [[nodiscard]] virtual RuntimeTerminalBackendResult WriteInput(
            std::span<const uint8_t> input) = 0;
        [[nodiscard]] virtual RuntimeTerminalBackendResult Resize(
            uint32_t columns,
            uint32_t rows) = 0;
        [[nodiscard]] virtual RuntimeTerminalBackendResult Interrupt() = 0;
    };
}
