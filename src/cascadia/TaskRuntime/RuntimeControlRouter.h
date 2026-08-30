// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolHandshake.h"
#include "ProtocolRpc.h"

#include <atomic>
#include <cstdint>
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

    enum class RuntimeControlSubscriptionAction
    {
        Bind,
        Unbind,
    };

    struct RuntimeControlSubscriptionChange
    {
        RuntimeControlSubscriptionAction Action{ RuntimeControlSubscriptionAction::Bind };
        std::string SubscriptionId;
        uint64_t Generation{};
        uint64_t Cursor{};
    };

    struct RuntimeControlSubscriptionPlan
    {
        std::string SubscriptionId;
        uint64_t Generation{};
    };

    struct RuntimeControlRouteResult
    {
        bool Succeeded{};
        std::string ResultJson{ "{}" };
        std::optional<Microsoft::Terminal::TaskRuntimeProtocol::ProtocolRpcError> Error;
        std::optional<RuntimeControlSubscriptionChange> SubscriptionChange;
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

        // Planning only allocates identity. SnapshotAndSubscribe Route runs
        // after the Runtime has installed a pending transport reservation, so
        // Route may atomically register its event source while creating the
        // snapshot without losing notifications.
        [[nodiscard]] virtual std::optional<RuntimeControlSubscriptionPlan> PlanSubscription(
            const RuntimeControlClientContext&,
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlRequest&,
            const RuntimeControlCancellationToken&)
        {
            return std::nullopt;
        }

        virtual void CancelSubscriptionPlan(
            const RuntimeControlClientContext&,
            const RuntimeControlSubscriptionPlan&) noexcept
        {
        }

        virtual void ClientDisconnected(const RuntimeControlClientContext&) noexcept
        {
        }

        // Implementations may be called concurrently for different clients.
        // Long-running work must observe cancellation and bound its own I/O.
        [[nodiscard]] virtual RuntimeControlRouteResult Route(
            const RuntimeControlClientContext& client,
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation) = 0;
    };
}
