// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalChannel.h"

#include "RuntimePipeIo.h"

namespace Microsoft::Terminal::TaskRuntime
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

            [[nodiscard]] HANDLE Release() noexcept
            {
                return std::exchange(Value, INVALID_HANDLE_VALUE);
            }
        };

        bool _validTimeout(const uint32_t timeoutMilliseconds) noexcept
        {
            return timeoutMilliseconds != 0 &&
                   timeoutMilliseconds <= MaximumRuntimeTransportTimeoutMilliseconds;
        }

        bool _validMaximumPayload(const uint32_t maximumPayloadBytes) noexcept
        {
            return maximumPayloadBytes != 0 &&
                   maximumPayloadBytes <=
                       Microsoft::Terminal::TaskRuntimeProtocol::MaximumTerminalFrameBytes;
        }

        RuntimeTransportResult _waitForTerminalPipe(
            const RuntimeTerminalEndpoint& endpoint,
            const uint64_t deadline,
            const HANDLE cancellationEvent)
        {
            constexpr DWORD waitSliceMilliseconds{ 50 };
            for (;;)
            {
                if (const auto cancelled = details::RuntimeCancellationFailure(
                        cancellationEvent))
                {
                    return *cancelled;
                }
                const auto now = GetTickCount64();
                if (now >= deadline)
                {
                    return details::RuntimeTransportFailure(
                        RuntimeTransportStatus::Timeout,
                        "Terminal pipe wait timed out",
                        WAIT_TIMEOUT);
                }
                if (WaitNamedPipeW(
                        endpoint.PipeName.c_str(),
                        static_cast<DWORD>((std::min)(
                            deadline - now,
                            uint64_t{ waitSliceMilliseconds }))))
                {
                    RuntimeTransportResult result;
                    result.Status = RuntimeTransportStatus::Succeeded;
                    return result;
                }
                const auto error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND)
                {
                    const auto retryNow = GetTickCount64();
                    if (retryNow >= deadline)
                    {
                        continue;
                    }
                    const auto retryMilliseconds = static_cast<DWORD>((std::min)(
                        deadline - retryNow,
                        uint64_t{ waitSliceMilliseconds }));
                    if (!cancellationEvent)
                    {
                        Sleep(retryMilliseconds);
                    }
                    else
                    {
                        const auto retry = WaitForSingleObject(
                            cancellationEvent,
                            retryMilliseconds);
                        if (retry == WAIT_OBJECT_0)
                        {
                            return details::RuntimeTransportFailure(
                                RuntimeTransportStatus::Cancelled,
                                "Terminal pipe wait was cancelled",
                                ERROR_OPERATION_ABORTED);
                        }
                        if (retry != WAIT_TIMEOUT)
                        {
                            return details::RuntimeTransportFailure(
                                RuntimeTransportStatus::SystemError,
                                "Terminal pipe cancellation wait failed",
                                GetLastError());
                        }
                    }
                    continue;
                }
                if (error != ERROR_SEM_TIMEOUT && error != ERROR_PIPE_BUSY)
                {
                    return details::RuntimeTransportFailure(
                        details::ClassifyRuntimeIoError(error),
                        "WaitNamedPipe failed for terminal endpoint",
                        error);
                }
            }
        }
    }

    RuntimeTerminalChannelResult ConnectRuntimeTerminalChannel(
        const RuntimeTerminalEndpoint& endpoint,
        const uint32_t maximumPayloadBytes,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        RuntimeTerminalChannelResult result;
        if (!ValidateRuntimeTerminalEndpoint(endpoint) ||
            !_validMaximumPayload(maximumPayloadBytes) ||
            !_validTimeout(timeoutMilliseconds))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Terminal endpoint, payload bound, or timeout is invalid";
            return result;
        }
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        for (;;)
        {
            const auto waited = _waitForTerminalPipe(
                endpoint,
                deadline,
                cancellationEvent);
            if (!waited.Succeeded())
            {
                static_cast<RuntimeTransportResult&>(result) = waited;
                return result;
            }

            HandleOwner pipe{ CreateFileW(
                endpoint.PipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                    SECURITY_IDENTIFICATION,
                nullptr) };
            if (pipe.Value == INVALID_HANDLE_VALUE)
            {
                const auto error = GetLastError();
                if (error == ERROR_PIPE_BUSY || error == ERROR_FILE_NOT_FOUND)
                {
                    continue;
                }
                result.Status = details::ClassifyRuntimeIoError(error);
                result.SystemError = error;
                result.Error = "CreateFile failed for terminal endpoint";
                return result;
            }
            DWORD mode{ PIPE_READMODE_BYTE };
            if (!SetNamedPipeHandleState(pipe.Value, &mode, nullptr, nullptr))
            {
                result.SystemError = GetLastError();
                result.Error = "SetNamedPipeHandleState failed for terminal endpoint";
                return result;
            }

            try
            {
                result.Channel = RuntimeTerminalChannel{
                    pipe.Value,
                    endpoint,
                    maximumPayloadBytes,
                };
                (void)pipe.Release();
                result.Status = RuntimeTransportStatus::Succeeded;
                return result;
            }
            catch (...)
            {
                result.Status = RuntimeTransportStatus::SystemError;
                result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
                result.Error = "Terminal channel allocation failed after connect";
                return result;
            }
        }
    }
}
