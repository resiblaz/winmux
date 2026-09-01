// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeTerminalBackend.h"

#include <functional>
#include <mutex>

namespace Microsoft::Terminal::TaskRuntime
{
    using RuntimeTaskTerminalInputHandler = std::function<RuntimeTerminalBackendResult(
        std::string_view taskId,
        std::span<const uint8_t> input)>;
    using RuntimeTaskTerminalInterruptHandler = std::function<RuntimeTerminalBackendResult(
        std::string_view taskId)>;

    class RuntimeTaskTerminalBackend final : public IRuntimeTerminalBackend
    {
    public:
        RuntimeTaskTerminalBackend(
            std::string taskId,
            std::vector<uint8_t> initialSnapshot,
            RuntimeTaskTerminalInputHandler input,
            RuntimeTaskTerminalInterruptHandler interrupt) noexcept;

        [[nodiscard]] RuntimeTerminalBackendSnapshotResult Snapshot() override;
        [[nodiscard]] RuntimeTerminalBackendResult WriteInput(std::span<const uint8_t> input) override;
        [[nodiscard]] RuntimeTerminalBackendResult Resize(uint32_t columns, uint32_t rows) override;
        [[nodiscard]] RuntimeTerminalBackendResult Interrupt() override;

        void AppendOutput(std::span<const uint8_t> output) noexcept;

    private:
        static constexpr size_t MaximumSnapshotBytes{ 512 * 1024 };

        std::string _taskId;
        std::vector<uint8_t> _snapshot;
        RuntimeTaskTerminalInputHandler _input;
        RuntimeTaskTerminalInterruptHandler _interrupt;
        std::mutex _mutex;
        uint32_t _columns{ 80 };
        uint32_t _rows{ 24 };
    };
}
