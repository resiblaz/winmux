// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlChannel.h"
#include "RuntimeControlQueue.h"
#include "RuntimeControlRouter.h"
#include "RuntimeControlSubscriptions.h"

#include "ControlSession.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeControlSessionConfiguration
    {
        Microsoft::Terminal::TaskRuntimeProtocol::RuntimeHandshakeConfiguration Handshake;
        uint32_t HandshakeTimeoutMilliseconds{ 5000 };
        uint32_t ReadTimeoutMilliseconds{ MaximumRuntimeTransportTimeoutMilliseconds };
        uint32_t WriteTimeoutMilliseconds{ 5000 };
        size_t MaximumInflightRequests{
            Microsoft::Terminal::TaskRuntimeProtocol::DefaultMaximumInflightRequests
        };
        size_t MaximumQueuedMessages{ DefaultMaximumQueuedControlMessages };
        size_t MaximumQueuedBytes{ DefaultMaximumQueuedControlBytes };
        size_t MaximumSubscriptions{ DefaultMaximumSubscriptionsPerControlClient };
        // Borrowed event used to wake the server acceptor for prompt reaping.
        HANDLE CompletionEvent{};
    };

    enum class RuntimeControlSessionCloseReason
    {
        None,
        ServerStopping,
        PeerDisconnected,
        HandshakeRejected,
        MalformedRequest,
        IdleTimeout,
        OutboundQueueLimit,
        WriteTimeout,
        TransportError,
    };

    class RuntimeControlSession final
    {
    public:
        RuntimeControlSession(
            RuntimeControlChannel channel,
            std::shared_ptr<IRuntimeControlRequestRouter> router,
            RuntimeControlSessionConfiguration configuration,
            std::shared_ptr<std::mutex> subscriptionOwnershipMutex);
        ~RuntimeControlSession();

        RuntimeControlSession(const RuntimeControlSession&) = delete;
        RuntimeControlSession& operator=(const RuntimeControlSession&) = delete;

        [[nodiscard]] bool Start();
        void RequestStop(RuntimeControlSessionCloseReason reason) noexcept;
        void Join() noexcept;

        [[nodiscard]] bool Finished() const noexcept;
        [[nodiscard]] RuntimeControlSessionCloseReason CloseReason() const noexcept;
        [[nodiscard]] bool OwnsSubscription(
            const std::string& subscriptionId,
            uint64_t generation) const;
        [[nodiscard]] RuntimeControlNotificationStatus PublishNotification(
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlNotification& notification);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
