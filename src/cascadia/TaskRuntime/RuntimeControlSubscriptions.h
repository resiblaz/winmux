// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlQueue.h"
#include "RuntimeControlRouter.h"

#include "ProtocolRpc.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr size_t DefaultMaximumSubscriptionsPerControlClient{ 64 };
    inline constexpr size_t AbsoluteMaximumSubscriptionsPerControlClient{ 256 };

    enum class RuntimeControlNotificationStatus
    {
        Delivered,
        Duplicate,
        NotSubscribed,
        SequenceGap,
        InvalidNotification,
        QueueLimitReached,
        Closed,
    };

    enum class RuntimeControlSubscriptionPlanStatus
    {
        Accepted,
        Invalid,
        LimitReached,
        Conflict,
    };

    enum class RuntimeControlSubscriptionApplyStatus
    {
        Accepted,
        InvalidChange,
        SequenceGap,
        QueueLimitReached,
        Closed,
    };

    [[nodiscard]] bool IsValidRuntimeControlSubscriptionChange(
        Microsoft::Terminal::TaskRuntimeProtocol::ControlMethod method,
        const RuntimeControlSubscriptionChange& change) noexcept;

    class RuntimeControlSubscriptions final
    {
    public:
        RuntimeControlSubscriptions(
            size_t maximumSubscriptions,
            size_t maximumPendingMessages,
            size_t maximumPendingBytes,
            uint32_t maximumPayloadBytes);
        ~RuntimeControlSubscriptions();

        RuntimeControlSubscriptions(const RuntimeControlSubscriptions&) = delete;
        RuntimeControlSubscriptions& operator=(const RuntimeControlSubscriptions&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] bool ConfigureMaximumPayloadBytes(uint32_t maximumPayloadBytes) noexcept;
        [[nodiscard]] RuntimeControlSubscriptionPlanStatus Plan(
            const RuntimeControlSubscriptionPlan& plan);
        void CancelPlan(const RuntimeControlSubscriptionPlan& plan) noexcept;
        [[nodiscard]] RuntimeControlSubscriptionApplyStatus EnqueueResponseAndApply(
            RuntimeControlQueue& outbound,
            std::string responsePayload,
            std::optional<std::string> completionRequestId,
            const RuntimeControlSubscriptionChange& change);
        [[nodiscard]] bool Owns(
            const std::string& subscriptionId,
            uint64_t generation) const;
        [[nodiscard]] RuntimeControlNotificationStatus Publish(
            RuntimeControlQueue& outbound,
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlNotification& notification);
        void ClearAndClose(RuntimeControlQueue& outbound) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
