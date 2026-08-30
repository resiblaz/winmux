// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"
#include "RuntimePipe.h"

#include "ProtocolHandshake.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeControlChannelResult;

    inline constexpr uint32_t MaximumRuntimeTransportTimeoutMilliseconds{ 60000 };

    enum class RuntimeTransportStatus
    {
        Succeeded,
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

    class RuntimeControlChannel final
    {
    public:
        RuntimeControlChannel() noexcept;
        ~RuntimeControlChannel();

        RuntimeControlChannel(const RuntimeControlChannel&) = delete;
        RuntimeControlChannel& operator=(const RuntimeControlChannel&) = delete;
        RuntimeControlChannel(RuntimeControlChannel&&) noexcept;
        RuntimeControlChannel& operator=(RuntimeControlChannel&&) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] RuntimeTransportResult Send(
            std::string_view payload,
            uint32_t timeoutMilliseconds);
        [[nodiscard]] RuntimeReceiveResult Receive(uint32_t timeoutMilliseconds);
        [[nodiscard]] bool ConfigureMaximumPayloadBytes(uint32_t maximumPayloadBytes) noexcept;
        [[nodiscard]] HANDLE Handle() const noexcept;
        void Close() noexcept;

    private:
        struct Impl;
        explicit RuntimeControlChannel(HANDLE handle);
        friend struct RuntimeControlChannelResult;
        friend RuntimeControlChannelResult AcceptRuntimeControlChannel(
            RuntimeNamedPipe listener,
            uint32_t timeoutMilliseconds);
        friend RuntimeControlChannelResult ConnectRuntimeControlChannel(
            const RuntimeIdentity& identity,
            uint32_t timeoutMilliseconds);
        std::unique_ptr<Impl> _impl;
    };

    struct RuntimeControlChannelResult
    {
        RuntimeTransportStatus Status{ RuntimeTransportStatus::SystemError };
        RuntimeControlChannel Channel;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeTransportStatus::Succeeded;
        }
    };

    struct RuntimeClientHandshakeResult : RuntimeTransportResult
    {
        Microsoft::Terminal::TaskRuntimeProtocol::HelloResponse Response;
        Microsoft::Terminal::TaskRuntimeProtocol::HandshakeError HandshakeError{
            Microsoft::Terminal::TaskRuntimeProtocol::HandshakeError::InvalidRequest
        };
    };

    struct RuntimeServerHandshakeResult : RuntimeTransportResult
    {
        Microsoft::Terminal::TaskRuntimeProtocol::HandshakeDecision Decision;
    };

    [[nodiscard]] RuntimeControlChannelResult AcceptRuntimeControlChannel(
        RuntimeNamedPipe listener,
        uint32_t timeoutMilliseconds);
    [[nodiscard]] RuntimeControlChannelResult ConnectRuntimeControlChannel(
        const RuntimeIdentity& identity,
        uint32_t timeoutMilliseconds);
    [[nodiscard]] RuntimeClientHandshakeResult PerformRuntimeClientHandshake(
        RuntimeControlChannel& channel,
        const Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest& request,
        uint32_t timeoutMilliseconds);
    [[nodiscard]] RuntimeServerHandshakeResult PerformRuntimeServerHandshake(
        RuntimeControlChannel& channel,
        const Microsoft::Terminal::TaskRuntimeProtocol::RuntimeHandshakeConfiguration& runtime,
        uint32_t timeoutMilliseconds);
}
