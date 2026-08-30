// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolHandshake.h"
#include "ProtocolRpc.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr size_t DefaultMaximumInflightRequests{ 64 };
    inline constexpr size_t AbsoluteMaximumInflightRequests{ 256 };

    enum class ControlSessionState
    {
        AwaitingHello,
        Ready,
        Closing,
        Closed,
    };

    struct ControlSessionDecision
    {
        bool Accepted{};
        ProtocolRpcErrorCode Error{ ProtocolRpcErrorCode::InvalidRequest };
        std::string Message;
    };

    class ControlSession final
    {
    public:
        explicit ControlSession(
            RuntimeHandshakeConfiguration runtime,
            size_t maximumInflightRequests = DefaultMaximumInflightRequests);

        [[nodiscard]] HandshakeDecision AcceptHello(const HelloRequest& request);
        [[nodiscard]] ControlSessionDecision BeginRequest(const ControlRequest& request);
        [[nodiscard]] bool CompleteRequest(std::string_view requestId);
        void BeginClose() noexcept;
        void MarkClosed() noexcept;

        [[nodiscard]] ControlSessionState State() const noexcept;
        [[nodiscard]] size_t InflightRequestCount() const noexcept;
        [[nodiscard]] ProtocolCapabilitySet NegotiatedCapabilities() const noexcept;

    private:
        RuntimeHandshakeConfiguration _runtime;
        size_t _maximumInflightRequests{};
        ControlSessionState _state{ ControlSessionState::AwaitingHello };
        ProtocolCapabilitySet _negotiatedCapabilities{};
        std::unordered_set<std::string> _inflightRequests;
    };
}
