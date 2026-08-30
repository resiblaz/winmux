// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlTransport.h"
#include "RuntimePipe.h"
#include "RuntimeTerminalEndpoint.h"

#include "TerminalFrame.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeTerminalReceiveResult : RuntimeTransportResult
    {
        Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame Frame;
    };

    struct RuntimeTerminalChannelResult;

    class RuntimeTerminalChannel final
    {
    public:
        RuntimeTerminalChannel() noexcept;
        ~RuntimeTerminalChannel();

        RuntimeTerminalChannel(const RuntimeTerminalChannel&) = delete;
        RuntimeTerminalChannel& operator=(const RuntimeTerminalChannel&) = delete;
        RuntimeTerminalChannel(RuntimeTerminalChannel&&) noexcept;
        RuntimeTerminalChannel& operator=(RuntimeTerminalChannel&&) noexcept;

        [[nodiscard]] bool Connected() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] RuntimeTransportResult Send(
            const Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame& frame,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr);
        [[nodiscard]] RuntimeTerminalReceiveResult Receive(
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr);
        [[nodiscard]] HANDLE Handle() const noexcept;
        [[nodiscard]] uint32_t MaximumPayloadBytes() const noexcept;
        [[nodiscard]] const RuntimeTerminalEndpoint* Endpoint() const noexcept;
        void CancelPendingIo() noexcept;
        void Close() noexcept;

    private:
        struct Impl;
        RuntimeTerminalChannel(
            HANDLE handle,
            RuntimeTerminalEndpoint endpoint,
            uint32_t maximumPayloadBytes);
        friend struct RuntimeTerminalChannelResult;
        friend RuntimeTerminalChannelResult AcceptRuntimeTerminalChannel(
            RuntimeNamedPipe& listener,
            const RuntimeTerminalEndpoint& endpoint,
            uint32_t maximumPayloadBytes,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent);
        friend RuntimeTerminalChannelResult ConnectRuntimeTerminalChannel(
            const RuntimeTerminalEndpoint& endpoint,
            uint32_t maximumPayloadBytes,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent);
        std::unique_ptr<Impl> _impl;
    };

    struct RuntimeTerminalChannelResult : RuntimeTransportResult
    {
        RuntimeTerminalChannel Channel;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return RuntimeTransportResult::Succeeded() && Channel.Connected();
        }
    };

    [[nodiscard]] RuntimeTerminalChannelResult AcceptRuntimeTerminalChannel(
        RuntimeNamedPipe& listener,
        const RuntimeTerminalEndpoint& endpoint,
        uint32_t maximumPayloadBytes,
        uint32_t timeoutMilliseconds,
        HANDLE cancellationEvent = nullptr);
    [[nodiscard]] RuntimeTerminalChannelResult ConnectRuntimeTerminalChannel(
        const RuntimeTerminalEndpoint& endpoint,
        uint32_t maximumPayloadBytes,
        uint32_t timeoutMilliseconds,
        HANDLE cancellationEvent = nullptr);
}
