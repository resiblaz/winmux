// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeTerminalChannel.h"

#include "ProtocolHandshake.h"

#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeTerminalClientHandshakeResult : RuntimeTransportResult
    {
        Microsoft::Terminal::TaskRuntimeProtocol::HelloResponse Response;
        Microsoft::Terminal::TaskRuntimeProtocol::HandshakeError HandshakeError{
            Microsoft::Terminal::TaskRuntimeProtocol::HandshakeError::InvalidRequest
        };
    };

    struct RuntimeTerminalServerHandshakeResult : RuntimeTransportResult
    {
        Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest Request;
        Microsoft::Terminal::TaskRuntimeProtocol::HandshakeDecision Decision;
    };

    [[nodiscard]] RuntimeTerminalClientHandshakeResult
        PerformRuntimeTerminalClientHandshake(
            RuntimeTerminalChannel& channel,
            const Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest& request,
            std::string_view expectedRuntimeInstanceId,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr);
    [[nodiscard]] RuntimeTerminalServerHandshakeResult
        PerformRuntimeTerminalServerHandshake(
            RuntimeTerminalChannel& channel,
            const Microsoft::Terminal::TaskRuntimeProtocol::RuntimeHandshakeConfiguration& runtime,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr);
}
