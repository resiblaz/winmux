// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlChannel.h"

#include "ProtocolHandshake.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    class ControlSession;
}

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeClientHandshakeResult : RuntimeTransportResult
    {
        Microsoft::Terminal::TaskRuntimeProtocol::HelloResponse Response;
        Microsoft::Terminal::TaskRuntimeProtocol::HandshakeError HandshakeError{
            Microsoft::Terminal::TaskRuntimeProtocol::HandshakeError::InvalidRequest
        };
    };

    struct RuntimeServerHandshakeResult : RuntimeTransportResult
    {
        Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest Request;
        Microsoft::Terminal::TaskRuntimeProtocol::HandshakeDecision Decision;
    };

    [[nodiscard]] RuntimeClientHandshakeResult PerformRuntimeClientHandshake(
        RuntimeControlChannel& channel,
        const Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest& request,
        uint32_t timeoutMilliseconds,
        HANDLE cancellationEvent = nullptr);
    [[nodiscard]] RuntimeServerHandshakeResult PerformRuntimeServerHandshake(
        RuntimeControlChannel& channel,
        const Microsoft::Terminal::TaskRuntimeProtocol::RuntimeHandshakeConfiguration& runtime,
        uint32_t timeoutMilliseconds,
        HANDLE cancellationEvent = nullptr);
    [[nodiscard]] RuntimeServerHandshakeResult PerformRuntimeServerHandshake(
        RuntimeControlChannel& channel,
        Microsoft::Terminal::TaskRuntimeProtocol::ControlSession& session,
        uint32_t timeoutMilliseconds,
        HANDLE cancellationEvent = nullptr);
}
