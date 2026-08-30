// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlTransport.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <span>

namespace Microsoft::Terminal::TaskRuntime::details
{
    [[nodiscard]] RuntimeTransportResult RuntimeTransportFailure(
        RuntimeTransportStatus status,
        std::string error,
        DWORD systemError = ERROR_SUCCESS);
    [[nodiscard]] RuntimeTransportStatus ClassifyRuntimeIoError(DWORD error) noexcept;
    [[nodiscard]] std::optional<RuntimeTransportResult> RuntimeCancellationFailure(
        HANDLE cancellationEvent);
    [[nodiscard]] std::optional<RuntimeTransportResult> RuntimeGenerationCancellationFailure(
        const std::atomic_uint64_t& cancellationGeneration,
        uint64_t expectedGeneration);
    [[nodiscard]] RuntimeTransportResult WaitRuntimeOverlapped(
        HANDLE handle,
        OVERLAPPED& operation,
        uint32_t timeoutMilliseconds,
        DWORD& transferred,
        HANDLE cancellationEvent);
    [[nodiscard]] RuntimeTransportResult WriteRuntimeControl(
        HANDLE handle,
        std::span<const uint8_t> bytes,
        uint32_t timeoutMilliseconds,
        HANDLE cancellationEvent,
        std::mutex& ioStartMutex,
        const std::atomic_uint64_t& cancellationGeneration,
        uint64_t expectedGeneration);
    [[nodiscard]] RuntimeTransportResult ReadRuntimeControl(
        HANDLE handle,
        std::span<uint8_t> buffer,
        uint32_t timeoutMilliseconds,
        DWORD& transferred,
        HANDLE cancellationEvent,
        std::mutex& ioStartMutex,
        const std::atomic_uint64_t& cancellationGeneration,
        uint64_t expectedGeneration);
}
