// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr uint32_t MaximumRuntimeTransportTimeoutMilliseconds{ 60000 };

    enum class RuntimeTransportStatus
    {
        Succeeded,
        Cancelled,
        Timeout,
        Disconnected,
        MalformedFrame,
        InvalidConfiguration,
        ProtocolRejected,
        SystemError,
    };

    struct RuntimeTransportResult
    {
        RuntimeTransportStatus Status{ RuntimeTransportStatus::SystemError };
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeTransportStatus::Succeeded;
        }
    };

    struct RuntimeReceiveResult : RuntimeTransportResult
    {
        std::string Payload;
    };

    // One reader and one writer may operate concurrently. CancelPendingIo
    // terminates the connection epoch and may interrupt either operation;
    // Close/destruction must wait until both callers have returned.
    class IRuntimeControlConnection
    {
    public:
        virtual ~IRuntimeControlConnection() = default;

        [[nodiscard]] virtual bool Connected() const noexcept = 0;
        [[nodiscard]] virtual RuntimeTransportResult Send(
            std::string_view payload,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr) = 0;
        [[nodiscard]] virtual RuntimeReceiveResult Receive(
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr) = 0;
        // A broad cancellation may leave a partial byte-stream frame. The
        // connection is not reusable after this call; reconnect for a new epoch.
        virtual void CancelPendingIo() noexcept = 0;
        virtual void Close() noexcept = 0;
    };
}
