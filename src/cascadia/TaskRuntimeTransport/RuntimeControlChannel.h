// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"
#include "RuntimePipe.h"
#include "RuntimeControlTransport.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeControlChannelResult;

    class RuntimeControlChannel final : public IRuntimeControlConnection
    {
    public:
        RuntimeControlChannel() noexcept;
        ~RuntimeControlChannel() override;

        RuntimeControlChannel(const RuntimeControlChannel&) = delete;
        RuntimeControlChannel& operator=(const RuntimeControlChannel&) = delete;
        RuntimeControlChannel(RuntimeControlChannel&&) noexcept;
        RuntimeControlChannel& operator=(RuntimeControlChannel&&) noexcept;

        [[nodiscard]] bool Connected() const noexcept override;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] RuntimeTransportResult Send(
            std::string_view payload,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr) override;
        [[nodiscard]] RuntimeReceiveResult Receive(
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr) override;
        [[nodiscard]] bool ConfigureMaximumPayloadBytes(uint32_t maximumPayloadBytes) noexcept;
        [[nodiscard]] HANDLE Handle() const noexcept;
        void CancelPendingIo() noexcept override;
        void Close() noexcept override;

    private:
        struct Impl;
        explicit RuntimeControlChannel(HANDLE handle);
        friend struct RuntimeControlChannelResult;
        friend RuntimeControlChannelResult AcceptRuntimeControlChannel(
            RuntimeNamedPipe& listener,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent);
        friend RuntimeControlChannelResult ConnectRuntimeControlChannel(
            const RuntimeIdentity& identity,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent);
        std::unique_ptr<Impl> _impl;
    };

    struct RuntimeControlChannelResult : RuntimeTransportResult
    {
        RuntimeControlChannel Channel;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return RuntimeTransportResult::Succeeded() &&
                   Channel.Connected();
        }
    };

    [[nodiscard]] RuntimeControlChannelResult AcceptRuntimeControlChannel(
        RuntimeNamedPipe& listener,
        uint32_t timeoutMilliseconds,
        HANDLE cancellationEvent = nullptr);
    [[nodiscard]] RuntimeControlChannelResult ConnectRuntimeControlChannel(
        const RuntimeIdentity& identity,
        uint32_t timeoutMilliseconds,
        HANDLE cancellationEvent = nullptr);
}
