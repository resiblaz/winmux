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
    }

    RuntimeTerminalChannelResult AcceptRuntimeTerminalChannel(
        RuntimeNamedPipe& listener,
        const RuntimeTerminalEndpoint& endpoint,
        const uint32_t maximumPayloadBytes,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        RuntimeTerminalChannelResult result;
        if (!listener || listener.Name() != endpoint.PipeName ||
            !ValidateRuntimeTerminalEndpoint(endpoint) ||
            !_validMaximumPayload(maximumPayloadBytes) ||
            !_validTimeout(timeoutMilliseconds))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error =
                "Terminal listener, endpoint, payload bound, or timeout is invalid";
            return result;
        }
        if (const auto cancelled = details::RuntimeCancellationFailure(
                cancellationEvent))
        {
            static_cast<RuntimeTransportResult&>(result) = *cancelled;
            return result;
        }

        HandleOwner event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        if (!event.Value)
        {
            result.SystemError = GetLastError();
            result.Error = "CreateEvent failed for terminal accept";
            return result;
        }
        OVERLAPPED operation{};
        operation.hEvent = event.Value;
        DWORD transferred{};
        const auto connected = ConnectNamedPipe(listener.Handle(), &operation);
        if (!connected)
        {
            const auto error = GetLastError();
            if (error == ERROR_IO_PENDING)
            {
                const auto waited = details::WaitRuntimeOverlapped(
                    listener.Handle(),
                    operation,
                    timeoutMilliseconds,
                    transferred,
                    cancellationEvent);
                if (!waited.Succeeded())
                {
                    static_cast<RuntimeTransportResult&>(result) = waited;
                    return result;
                }
            }
            else if (error != ERROR_PIPE_CONNECTED)
            {
                result.Status = details::ClassifyRuntimeIoError(error);
                result.SystemError = error;
                result.Error = "ConnectNamedPipe failed for terminal pipe";
                return result;
            }
        }

        try
        {
            result.Channel = RuntimeTerminalChannel{
                listener.Handle(),
                endpoint,
                maximumPayloadBytes,
            };
            (void)listener.Release();
            result.Status = RuntimeTransportStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            result.Status = RuntimeTransportStatus::SystemError;
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error = "Terminal channel allocation failed after accept";
            return result;
        }
    }
}
