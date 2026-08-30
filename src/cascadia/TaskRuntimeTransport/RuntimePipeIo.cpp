// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimePipeIo.h"

// Shared overlapped byte-stream I/O for both control and terminal pipes.

namespace Microsoft::Terminal::TaskRuntime::details
{
    namespace
    {
        struct HandleOwner
        {
            HANDLE Value{ INVALID_HANDLE_VALUE };

            ~HandleOwner()
            {
                if (Value != INVALID_HANDLE_VALUE && Value != nullptr)
                {
                    CloseHandle(Value);
                }
            }
        };

        void _cancelOverlapped(const HANDLE handle, OVERLAPPED& operation) noexcept
        {
            CancelIoEx(handle, &operation);
            DWORD ignored{};
            GetOverlappedResult(handle, &operation, &ignored, TRUE);
        }
    }

    RuntimeTransportResult RuntimeTransportFailure(
        const RuntimeTransportStatus status,
        std::string error,
        const DWORD systemError)
    {
        RuntimeTransportResult result;
        result.Status = status;
        result.SystemError = systemError;
        result.Error = std::move(error);
        return result;
    }

    RuntimeTransportStatus ClassifyRuntimeIoError(const DWORD error) noexcept
    {
        if (error == ERROR_OPERATION_ABORTED)
        {
            return RuntimeTransportStatus::Cancelled;
        }
        return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                       error == ERROR_NO_DATA ?
                   RuntimeTransportStatus::Disconnected :
                   RuntimeTransportStatus::SystemError;
    }

    std::optional<RuntimeTransportResult> RuntimeCancellationFailure(
        const HANDLE cancellationEvent)
    {
        if (!cancellationEvent)
        {
            return std::nullopt;
        }
        const auto wait = WaitForSingleObject(cancellationEvent, 0);
        if (wait == WAIT_TIMEOUT)
        {
            return std::nullopt;
        }
        if (wait == WAIT_OBJECT_0)
        {
            return RuntimeTransportFailure(
                RuntimeTransportStatus::Cancelled,
                "Named-pipe operation was cancelled",
                ERROR_OPERATION_ABORTED);
        }
        return RuntimeTransportFailure(
            RuntimeTransportStatus::SystemError,
            "Cancellation event wait failed",
            GetLastError());
    }

    std::optional<RuntimeTransportResult> RuntimeGenerationCancellationFailure(
        const std::atomic_uint64_t& cancellationGeneration,
        const uint64_t expectedGeneration)
    {
        return cancellationGeneration.load(std::memory_order_acquire) == expectedGeneration ?
                   std::nullopt :
                   std::optional{ RuntimeTransportFailure(
                       RuntimeTransportStatus::Cancelled,
                       "Named-pipe operation was cancelled",
                       ERROR_OPERATION_ABORTED) };
    }

    RuntimeTransportResult WaitRuntimeOverlapped(
        const HANDLE handle,
        OVERLAPPED& operation,
        const uint32_t timeoutMilliseconds,
        DWORD& transferred,
        const HANDLE cancellationEvent)
    {
        const std::array handles{ operation.hEvent, cancellationEvent };
        const auto wait = WaitForMultipleObjects(
            cancellationEvent ? 2 : 1,
            handles.data(),
            FALSE,
            timeoutMilliseconds);
        if (wait == WAIT_TIMEOUT)
        {
            _cancelOverlapped(handle, operation);
            return RuntimeTransportFailure(
                RuntimeTransportStatus::Timeout,
                "Named-pipe operation timed out",
                WAIT_TIMEOUT);
        }
        if (wait == WAIT_OBJECT_0 + 1)
        {
            _cancelOverlapped(handle, operation);
            return RuntimeTransportFailure(
                RuntimeTransportStatus::Cancelled,
                "Named-pipe operation was cancelled",
                ERROR_OPERATION_ABORTED);
        }
        if (wait != WAIT_OBJECT_0)
        {
            const auto error = GetLastError();
            _cancelOverlapped(handle, operation);
            return RuntimeTransportFailure(
                RuntimeTransportStatus::SystemError,
                "Named-pipe wait failed",
                error);
        }
        if (!GetOverlappedResult(handle, &operation, &transferred, FALSE))
        {
            const auto error = GetLastError();
            return RuntimeTransportFailure(
                ClassifyRuntimeIoError(error),
                "Named-pipe operation failed",
                error);
        }
        RuntimeTransportResult result;
        result.Status = RuntimeTransportStatus::Succeeded;
        return result;
    }

    RuntimeTransportResult WriteRuntimePipe(
        const HANDLE handle,
        const std::span<const uint8_t> bytes,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent,
        std::mutex& ioStartMutex,
        const std::atomic_uint64_t& cancellationGeneration,
        const uint64_t expectedGeneration)
    {
        size_t offset{};
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        while (offset < bytes.size())
        {
            if (const auto cancelled = RuntimeCancellationFailure(cancellationEvent))
            {
                return *cancelled;
            }
            if (const auto cancelled = RuntimeGenerationCancellationFailure(
                    cancellationGeneration,
                    expectedGeneration))
            {
                return *cancelled;
            }
            HandleOwner event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            if (!event.Value)
            {
                return RuntimeTransportFailure(
                    RuntimeTransportStatus::SystemError,
                    "CreateEvent failed",
                    GetLastError());
            }
            OVERLAPPED operation{};
            operation.hEvent = event.Value;
            DWORD transferred{};
            const auto remainingBytes = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, MAXDWORD));
            BOOL started{};
            DWORD startError{};
            {
                std::scoped_lock startLock{ ioStartMutex };
                if (const auto cancelled = RuntimeGenerationCancellationFailure(
                        cancellationGeneration,
                        expectedGeneration))
                {
                    return *cancelled;
                }
                if (const auto cancelled = RuntimeCancellationFailure(cancellationEvent))
                {
                    return *cancelled;
                }
                started = WriteFile(
                    handle,
                    bytes.data() + offset,
                    remainingBytes,
                    &transferred,
                    &operation);
                if (!started)
                {
                    startError = GetLastError();
                }
            }
            if (!started)
            {
                if (startError != ERROR_IO_PENDING)
                {
                    return RuntimeTransportFailure(
                        ClassifyRuntimeIoError(startError),
                        "Named-pipe write failed",
                        startError);
                }
                const auto now = GetTickCount64();
                if (now >= deadline)
                {
                    _cancelOverlapped(handle, operation);
                    return RuntimeTransportFailure(
                        RuntimeTransportStatus::Timeout,
                        "Named-pipe write timed out",
                        WAIT_TIMEOUT);
                }
                const auto waited = WaitRuntimeOverlapped(
                    handle,
                    operation,
                    static_cast<uint32_t>(deadline - now),
                    transferred,
                    cancellationEvent);
                if (!waited.Succeeded())
                {
                    return waited;
                }
            }
            if (transferred == 0)
            {
                return RuntimeTransportFailure(
                    RuntimeTransportStatus::Disconnected,
                    "Named-pipe write completed with zero bytes");
            }
            offset += transferred;
        }
        RuntimeTransportResult result;
        result.Status = RuntimeTransportStatus::Succeeded;
        return result;
    }

    RuntimeTransportResult ReadRuntimePipe(
        const HANDLE handle,
        const std::span<uint8_t> buffer,
        const uint32_t timeoutMilliseconds,
        DWORD& transferred,
        const HANDLE cancellationEvent,
        std::mutex& ioStartMutex,
        const std::atomic_uint64_t& cancellationGeneration,
        const uint64_t expectedGeneration)
    {
        if (const auto cancelled = RuntimeCancellationFailure(cancellationEvent))
        {
            return *cancelled;
        }
        if (const auto cancelled = RuntimeGenerationCancellationFailure(
                cancellationGeneration,
                expectedGeneration))
        {
            return *cancelled;
        }
        HandleOwner event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        if (!event.Value)
        {
            return RuntimeTransportFailure(
                RuntimeTransportStatus::SystemError,
                "CreateEvent failed",
                GetLastError());
        }
        OVERLAPPED operation{};
        operation.hEvent = event.Value;
        transferred = 0;
        BOOL started{};
        DWORD startError{};
        {
            std::scoped_lock startLock{ ioStartMutex };
            if (const auto cancelled = RuntimeGenerationCancellationFailure(
                    cancellationGeneration,
                    expectedGeneration))
            {
                return *cancelled;
            }
            if (const auto cancelled = RuntimeCancellationFailure(cancellationEvent))
            {
                return *cancelled;
            }
            started = ReadFile(
                handle,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &transferred,
                &operation);
            if (!started)
            {
                startError = GetLastError();
            }
        }
        if (!started)
        {
            if (startError != ERROR_IO_PENDING)
            {
                return RuntimeTransportFailure(
                    ClassifyRuntimeIoError(startError),
                    "Named-pipe read failed",
                    startError);
            }
            return WaitRuntimeOverlapped(
                handle,
                operation,
                timeoutMilliseconds,
                transferred,
                cancellationEvent);
        }
        RuntimeTransportResult result;
        result.Status = RuntimeTransportStatus::Succeeded;
        return result;
    }
}
