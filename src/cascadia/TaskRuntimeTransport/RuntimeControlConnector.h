// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlTransport.h"
#include "RuntimeIdentity.h"

#include "ProtocolHandshake.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeControlConnectPhase
    {
        Connecting,
        Handshaking,
        Connected,
    };

    struct RuntimeControlConnectionResult : RuntimeTransportResult
    {
        RuntimeControlConnectPhase Phase{ RuntimeControlConnectPhase::Connecting };
        std::unique_ptr<IRuntimeControlConnection> Connection;
        Microsoft::Terminal::TaskRuntimeProtocol::HelloResponse Response;
        Microsoft::Terminal::TaskRuntimeProtocol::HandshakeError HandshakeError{
            Microsoft::Terminal::TaskRuntimeProtocol::HandshakeError::InvalidRequest
        };

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return RuntimeTransportResult::Succeeded() &&
                   Phase == RuntimeControlConnectPhase::Connected &&
                   Connection && Connection->Connected();
        }
    };

    class IRuntimeControlConnector
    {
    public:
        virtual ~IRuntimeControlConnector() = default;

        [[nodiscard]] virtual RuntimeControlConnectionResult Connect(
            const RuntimeIdentity& identity,
            const Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest& request,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr) noexcept = 0;
    };

    // Synchronous single-attempt connector. It owns no worker threads and does
    // not retry; a client supervisor decides when another attempt is safe.
    class RuntimeNamedPipeControlConnector final : public IRuntimeControlConnector
    {
    public:
        [[nodiscard]] RuntimeControlConnectionResult Connect(
            const RuntimeIdentity& identity,
            const Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest& request,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr) noexcept override;
    };
}
