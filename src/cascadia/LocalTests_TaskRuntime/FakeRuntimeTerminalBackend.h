// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeTerminalBackend.h"

#include <stdexcept>
#include <utility>

namespace TaskRuntimeLocalTests
{
    class FakeRuntimeTerminalBackend final :
        public Microsoft::Terminal::TaskRuntime::IRuntimeTerminalBackend
    {
    public:
        Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendSnapshotResult Snapshot() override
        {
            ++SnapshotCalls;
            if (ThrowOnSnapshot)
            {
                throw std::runtime_error{ "fake snapshot failure" };
            }

            Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendSnapshotResult result;
            result.Status = SnapshotStatus;
            result.Error = SnapshotError;
            result.Payload = SnapshotPayload;
            return result;
        }

        Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendResult WriteInput(
            const std::span<const uint8_t> input) override
        {
            ++WriteInputCalls;
            if (ThrowOnWriteInput)
            {
                throw std::runtime_error{ "fake input failure" };
            }
            Inputs.emplace_back(input.begin(), input.end());
            return _result(WriteInputStatus, WriteInputError);
        }

        Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendResult Resize(
            const uint32_t columns,
            const uint32_t rows) override
        {
            ++ResizeCalls;
            if (ThrowOnResize)
            {
                throw std::runtime_error{ "fake resize failure" };
            }
            Sizes.emplace_back(columns, rows);
            return _result(ResizeStatus, ResizeError);
        }

        Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendResult Interrupt() override
        {
            ++InterruptCalls;
            if (ThrowOnInterrupt)
            {
                throw std::runtime_error{ "fake interrupt failure" };
            }
            return _result(InterruptStatus, InterruptError);
        }

        std::vector<uint8_t> SnapshotPayload{ 's', 'n', 'a', 'p' };
        Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendStatus SnapshotStatus{
            Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendStatus::Succeeded
        };
        Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendStatus WriteInputStatus{
            Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendStatus::Succeeded
        };
        Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendStatus ResizeStatus{
            Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendStatus::Succeeded
        };
        Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendStatus InterruptStatus{
            Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendStatus::Succeeded
        };
        std::string SnapshotError;
        std::string WriteInputError;
        std::string ResizeError;
        std::string InterruptError;
        bool ThrowOnSnapshot{};
        bool ThrowOnWriteInput{};
        bool ThrowOnResize{};
        bool ThrowOnInterrupt{};
        uint32_t SnapshotCalls{};
        uint32_t WriteInputCalls{};
        uint32_t ResizeCalls{};
        uint32_t InterruptCalls{};
        std::vector<std::vector<uint8_t>> Inputs;
        std::vector<std::pair<uint32_t, uint32_t>> Sizes;

    private:
        static Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendResult _result(
            const Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendStatus status,
            const std::string& error)
        {
            Microsoft::Terminal::TaskRuntime::RuntimeTerminalBackendResult result;
            result.Status = status;
            result.Error = error;
            return result;
        }
    };
}
