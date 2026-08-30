// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolHandshake.h"
#include "ProtocolRpc.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeControlClientContext
    {
        std::string ClientInstanceId;
        Microsoft::Terminal::TaskRuntimeProtocol::ProtocolClientKind ClientKind{
            Microsoft::Terminal::TaskRuntimeProtocol::ProtocolClientKind::TerminalUi
        };
        Microsoft::Terminal::TaskRuntimeProtocol::ProtocolCapabilitySet NegotiatedCapabilities{};
    };

    struct RuntimeControlRouteResult
    {
        bool Succeeded{};
        std::string ResultJson{ "{}" };
        std::optional<Microsoft::Terminal::TaskRuntimeProtocol::ProtocolRpcError> Error;
    };

    class RuntimeControlCancellationToken final
    {
    public:
        explicit RuntimeControlCancellationToken(std::shared_ptr<std::atomic_bool> state) noexcept :
            _state{ std::move(state) }
        {
        }

        [[nodiscard]] bool IsCancellationRequested() const noexcept
        {
            return !_state || _state->load(std::memory_order_acquire);
        }

    private:
        std::shared_ptr<std::atomic_bool> _state;
    };

    class IRuntimeControlRequestRouter
    {
    public:
        virtual ~IRuntimeControlRequestRouter() = default;

        // Implementations may be called concurrently for different clients.
        // Long-running work must observe cancellation and bound its own I/O.
        [[nodiscard]] virtual RuntimeControlRouteResult Route(
            const RuntimeControlClientContext& client,
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation) = 0;
    };
}
