// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskTerminalBackend.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        RuntimeTerminalBackendResult _failure(std::string error)
        {
            return { RuntimeTerminalBackendStatus::Rejected, std::move(error) };
        }
    }

    RuntimeTaskTerminalBackend::RuntimeTaskTerminalBackend(
        std::string taskId,
        std::vector<uint8_t> initialSnapshot,
        RuntimeTaskTerminalInputHandler input,
        RuntimeTaskTerminalInterruptHandler interrupt) noexcept :
        _taskId{ std::move(taskId) },
        _snapshot{ std::move(initialSnapshot) },
        _input{ std::move(input) },
        _interrupt{ std::move(interrupt) }
    {
        if (_snapshot.empty())
        {
            constexpr std::string_view fallback{ "\x1b[2J\x1b[HTask terminal is ready.\r\n" };
            _snapshot.assign(fallback.begin(), fallback.end());
        }
        if (_snapshot.size() > MaximumSnapshotBytes)
        {
            _snapshot.erase(_snapshot.begin(), _snapshot.end() - MaximumSnapshotBytes);
        }
    }

    RuntimeTerminalBackendSnapshotResult RuntimeTaskTerminalBackend::Snapshot()
    {
        const std::scoped_lock lock{ _mutex };
        RuntimeTerminalBackendSnapshotResult result;
        result.Status = RuntimeTerminalBackendStatus::Succeeded;
        result.Payload = _snapshot;
        return result;
    }

    RuntimeTerminalBackendResult RuntimeTaskTerminalBackend::WriteInput(const std::span<const uint8_t> input)
    {
        if (!_input || input.empty())
        {
            return _failure("Task terminal input is unavailable");
        }
        return _input(_taskId, input);
    }

    RuntimeTerminalBackendResult RuntimeTaskTerminalBackend::Resize(
        const uint32_t columns,
        const uint32_t rows)
    {
        if (columns == 0 || rows == 0)
        {
            return _failure("Task terminal dimensions are invalid");
        }
        const std::scoped_lock lock{ _mutex };
        _columns = columns;
        _rows = rows;
        return { RuntimeTerminalBackendStatus::Succeeded, {} };
    }

    RuntimeTerminalBackendResult RuntimeTaskTerminalBackend::Interrupt()
    {
        return _interrupt ? _interrupt(_taskId) : _failure("Task terminal interrupt is unavailable");
    }

    void RuntimeTaskTerminalBackend::AppendOutput(const std::span<const uint8_t> output) noexcept
    try
    {
        if (output.empty())
        {
            return;
        }
        const std::scoped_lock lock{ _mutex };
        if (output.size() >= MaximumSnapshotBytes)
        {
            _snapshot.assign(output.end() - MaximumSnapshotBytes, output.end());
            return;
        }
        const auto retained = MaximumSnapshotBytes - output.size();
        if (_snapshot.size() > retained)
        {
            _snapshot.erase(_snapshot.begin(), _snapshot.end() - retained);
        }
        _snapshot.insert(_snapshot.end(), output.begin(), output.end());
    }
    catch (...)
    {
    }
}
