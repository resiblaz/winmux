// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlSession.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeControlSession::Impl
    {
        RuntimeControlChannel Channel;
        std::shared_ptr<IRuntimeControlRequestRouter> Router;
        RuntimeControlSessionConfiguration Configuration;
        Microsoft::Terminal::TaskRuntimeProtocol::ControlSession ProtocolSession;
        RuntimeControlQueue Outbound;
        RuntimeControlClientContext Client;
        mutable std::mutex ClientMutex;
        std::shared_ptr<std::atomic_bool> CancellationState{ std::make_shared<std::atomic_bool>(false) };
        std::shared_ptr<std::mutex> SubscriptionOwnershipMutex;
        std::atomic_uint32_t MaximumPayloadBytes{};
        mutable std::mutex ProtocolMutex;
        RuntimeControlSubscriptions Subscriptions;
        HANDLE CancellationEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        std::thread Reader;
        std::thread Writer;
        std::atomic_bool ReaderDone{};
        std::atomic_bool WriterDone{};
        std::atomic_bool Started{};
        std::atomic_bool RouterDisconnectNotified{};
        std::atomic<RuntimeControlSessionCloseReason> Reason{ RuntimeControlSessionCloseReason::None };

        Impl(
            RuntimeControlChannel channel,
            std::shared_ptr<IRuntimeControlRequestRouter> router,
            RuntimeControlSessionConfiguration configuration,
            std::shared_ptr<std::mutex> subscriptionOwnershipMutex);
        ~Impl();

        void RequestStop(RuntimeControlSessionCloseReason reason) noexcept;
        void CancelPlannedSubscription(
            std::optional<RuntimeControlSubscriptionPlan>& plannedSubscription,
            bool transportReservationInstalled) noexcept;
        [[nodiscard]] bool EnqueueResponse(
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlResponse& response,
            bool completeRequest,
            Microsoft::Terminal::TaskRuntimeProtocol::ControlMethod method =
                Microsoft::Terminal::TaskRuntimeProtocol::ControlMethod::Ping,
            std::optional<RuntimeControlSubscriptionChange> subscriptionChange = std::nullopt,
            std::optional<RuntimeControlSubscriptionPlan> plannedSubscription = std::nullopt);
        void RunReader() noexcept;
        void RunWriter() noexcept;
        void MarkClosed() noexcept;
        [[nodiscard]] bool OwnsSubscription(
            const std::string& subscriptionId,
            uint64_t generation) const;
        [[nodiscard]] RuntimeControlNotificationStatus PublishNotification(
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlNotification& notification);
    };
}
