// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlSubscriptions.h"

#include "ProtocolRpcJson.h"
#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

#include <limits>
#include <mutex>
#include <unordered_map>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    bool IsValidRuntimeControlSubscriptionChange(
        const ControlMethod method,
        const RuntimeControlSubscriptionChange& change) noexcept
    {
        if (!IsCanonicalProtocolUuid(change.SubscriptionId) || change.Generation == 0)
        {
            return false;
        }
        return (change.Action == RuntimeControlSubscriptionAction::Bind &&
                method == ControlMethod::SnapshotAndSubscribe) ||
               (change.Action == RuntimeControlSubscriptionAction::Unbind &&
                method == ControlMethod::Unsubscribe);
    }

    struct RuntimeControlSubscriptions::Impl
    {
        enum class State
        {
            Pending,
            Active,
        };

        struct PendingNotification
        {
            uint64_t Sequence{};
            ControlNotificationKind Kind{ ControlNotificationKind::ProjectionChanged };
            std::string EncodedPayload;
            size_t WireBytes{};
        };

        struct Subscription
        {
            State CurrentState{ State::Pending };
            uint64_t Generation{};
            uint64_t Cursor{};
            bool PendingClosed{};
            std::vector<PendingNotification> Pending;
        };

        size_t MaximumSubscriptions{};
        size_t MaximumPendingMessages{};
        size_t MaximumPendingBytes{};
        uint32_t MaximumPayloadBytes{};
        size_t PendingMessages{};
        size_t PendingBytes{};
        std::unordered_map<std::string, Subscription> Subscriptions;
        mutable std::mutex Mutex;

        [[nodiscard]] bool Valid() const noexcept
        {
            return MaximumSubscriptions != 0 &&
                   MaximumSubscriptions <= AbsoluteMaximumSubscriptionsPerControlClient &&
                   MaximumPendingMessages <= AbsoluteMaximumQueuedControlMessages &&
                   MaximumPendingBytes <= AbsoluteMaximumQueuedControlBytes &&
                   MaximumPayloadBytes >= MinimumControlFrameBytes &&
                   MaximumPayloadBytes <= MaximumControlFrameBytes;
        }

        void RemovePendingCounts(const Subscription& subscription) noexcept
        {
            for (const auto& notification : subscription.Pending)
            {
                if (PendingMessages != 0)
                {
                    --PendingMessages;
                }
                PendingBytes = notification.WireBytes <= PendingBytes ?
                                   PendingBytes - notification.WireBytes :
                                   0;
            }
        }
    };

    RuntimeControlSubscriptions::RuntimeControlSubscriptions(
        const size_t maximumSubscriptions,
        const size_t maximumPendingMessages,
        const size_t maximumPendingBytes,
        const uint32_t maximumPayloadBytes) :
        _impl{ std::make_unique<Impl>() }
    {
        _impl->MaximumSubscriptions = maximumSubscriptions;
        _impl->MaximumPendingMessages = maximumPendingMessages;
        _impl->MaximumPendingBytes = maximumPendingBytes;
        _impl->MaximumPayloadBytes = maximumPayloadBytes;
    }

    RuntimeControlSubscriptions::~RuntimeControlSubscriptions() = default;

    bool RuntimeControlSubscriptions::Valid() const noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        return _impl->Valid();
    }

    bool RuntimeControlSubscriptions::ConfigureMaximumPayloadBytes(
        const uint32_t maximumPayloadBytes) noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        if (!_impl->Subscriptions.empty() || maximumPayloadBytes < MinimumControlFrameBytes ||
            maximumPayloadBytes > MaximumControlFrameBytes)
        {
            return false;
        }
        _impl->MaximumPayloadBytes = maximumPayloadBytes;
        return true;
    }

    RuntimeControlSubscriptionPlanStatus RuntimeControlSubscriptions::Plan(
        const RuntimeControlSubscriptionPlan& plan)
    {
        std::scoped_lock lock{ _impl->Mutex };
        if (!_impl->Valid() || !IsCanonicalProtocolUuid(plan.SubscriptionId) ||
            plan.Generation == 0)
        {
            return RuntimeControlSubscriptionPlanStatus::Invalid;
        }
        if (_impl->Subscriptions.contains(plan.SubscriptionId))
        {
            return RuntimeControlSubscriptionPlanStatus::Conflict;
        }
        if (_impl->Subscriptions.size() >= _impl->MaximumSubscriptions)
        {
            return RuntimeControlSubscriptionPlanStatus::LimitReached;
        }

        Impl::Subscription subscription;
        subscription.Generation = plan.Generation;
        _impl->Subscriptions.emplace(plan.SubscriptionId, std::move(subscription));
        return RuntimeControlSubscriptionPlanStatus::Accepted;
    }

    void RuntimeControlSubscriptions::CancelPlan(
        const RuntimeControlSubscriptionPlan& plan) noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        const auto found = _impl->Subscriptions.find(plan.SubscriptionId);
        if (found != _impl->Subscriptions.end() &&
            found->second.CurrentState == Impl::State::Pending &&
            found->second.Generation == plan.Generation)
        {
            _impl->RemovePendingCounts(found->second);
            _impl->Subscriptions.erase(found);
        }
    }

    RuntimeControlSubscriptionApplyStatus RuntimeControlSubscriptions::EnqueueResponseAndApply(
        RuntimeControlQueue& outbound,
        std::string responsePayload,
        std::optional<std::string> completionRequestId,
        const RuntimeControlSubscriptionChange& change)
    {
        std::scoped_lock lock{ _impl->Mutex };
        if (!_impl->Valid() || !IsCanonicalProtocolUuid(change.SubscriptionId) ||
            change.Generation == 0)
        {
            return RuntimeControlSubscriptionApplyStatus::InvalidChange;
        }

        if (change.Action == RuntimeControlSubscriptionAction::Unbind)
        {
            const auto found = _impl->Subscriptions.find(change.SubscriptionId);
            if (found != _impl->Subscriptions.end() &&
                found->second.Generation == change.Generation)
            {
                _impl->RemovePendingCounts(found->second);
                _impl->Subscriptions.erase(found);
            }
            const auto queued = outbound.Enqueue(
                std::move(responsePayload),
                std::move(completionRequestId));
            return queued == RuntimeControlQueueStatus::Accepted ?
                       RuntimeControlSubscriptionApplyStatus::Accepted :
                   queued == RuntimeControlQueueStatus::QueueLimitReached ?
                       RuntimeControlSubscriptionApplyStatus::QueueLimitReached :
                   queued == RuntimeControlQueueStatus::Closed ?
                       RuntimeControlSubscriptionApplyStatus::Closed :
                       RuntimeControlSubscriptionApplyStatus::InvalidChange;
        }

        if (change.Action != RuntimeControlSubscriptionAction::Bind)
        {
            return RuntimeControlSubscriptionApplyStatus::InvalidChange;
        }
        const auto found = _impl->Subscriptions.find(change.SubscriptionId);
        if (found == _impl->Subscriptions.end() ||
            found->second.CurrentState != Impl::State::Pending ||
            found->second.Generation != change.Generation)
        {
            return RuntimeControlSubscriptionApplyStatus::InvalidChange;
        }

        uint64_t cursor = change.Cursor;
        for (const auto& pending : found->second.Pending)
        {
            if (pending.Sequence <= cursor)
            {
                continue;
            }
            if (cursor == std::numeric_limits<uint64_t>::max() ||
                pending.Sequence != cursor + 1)
            {
                return RuntimeControlSubscriptionApplyStatus::SequenceGap;
            }
            cursor = pending.Sequence;
        }

        std::vector<RuntimeControlQueueInput> batch;
        batch.reserve(found->second.Pending.size() + 1);
        batch.push_back({ std::move(responsePayload), std::move(completionRequestId) });
        for (auto& pending : found->second.Pending)
        {
            if (pending.Sequence > change.Cursor)
            {
                batch.push_back({ std::move(pending.EncodedPayload), std::nullopt });
            }
        }
        const auto queued = outbound.EnqueueBatch(std::move(batch));
        if (queued != RuntimeControlQueueStatus::Accepted)
        {
            return queued == RuntimeControlQueueStatus::QueueLimitReached ?
                       RuntimeControlSubscriptionApplyStatus::QueueLimitReached :
                   queued == RuntimeControlQueueStatus::Closed ?
                       RuntimeControlSubscriptionApplyStatus::Closed :
                       RuntimeControlSubscriptionApplyStatus::InvalidChange;
        }

        _impl->RemovePendingCounts(found->second);
        found->second.Pending.clear();
        found->second.Cursor = cursor;
        found->second.CurrentState = Impl::State::Active;
        if (found->second.PendingClosed)
        {
            _impl->Subscriptions.erase(found);
        }
        return RuntimeControlSubscriptionApplyStatus::Accepted;
    }

    bool RuntimeControlSubscriptions::Owns(
        const std::string& subscriptionId,
        const uint64_t generation) const
    {
        std::scoped_lock lock{ _impl->Mutex };
        const auto found = _impl->Subscriptions.find(subscriptionId);
        return found != _impl->Subscriptions.end() && found->second.Generation == generation;
    }

    RuntimeControlNotificationStatus RuntimeControlSubscriptions::Publish(
        RuntimeControlQueue& outbound,
        const ControlNotification& notification)
    {
        auto encoded = EncodeControlNotification(notification);
        if (!encoded.Succeeded)
        {
            return RuntimeControlNotificationStatus::InvalidNotification;
        }

        std::scoped_lock lock{ _impl->Mutex };
        if (encoded.Payload.size() > _impl->MaximumPayloadBytes)
        {
            return RuntimeControlNotificationStatus::InvalidNotification;
        }
        const auto found = _impl->Subscriptions.find(notification.SubscriptionId);
        if (found == _impl->Subscriptions.end() ||
            found->second.Generation != notification.Generation)
        {
            return RuntimeControlNotificationStatus::NotSubscribed;
        }

        if (found->second.CurrentState == Impl::State::Pending)
        {
            if (found->second.PendingClosed)
            {
                return RuntimeControlNotificationStatus::NotSubscribed;
            }
            if (!found->second.Pending.empty())
            {
                const auto last = found->second.Pending.back().Sequence;
                if (notification.Sequence <= last)
                {
                    return RuntimeControlNotificationStatus::Duplicate;
                }
                if (last == std::numeric_limits<uint64_t>::max() ||
                    notification.Sequence != last + 1)
                {
                    return RuntimeControlNotificationStatus::SequenceGap;
                }
            }
            const auto wireBytes = encoded.Payload.size() + sizeof(uint32_t);
            if (_impl->PendingMessages >= _impl->MaximumPendingMessages ||
                _impl->PendingBytes > _impl->MaximumPendingBytes ||
                wireBytes > _impl->MaximumPendingBytes - _impl->PendingBytes)
            {
                return RuntimeControlNotificationStatus::QueueLimitReached;
            }
            found->second.Pending.push_back({
                notification.Sequence,
                notification.Kind,
                std::move(encoded.Payload),
                wireBytes,
            });
            ++_impl->PendingMessages;
            _impl->PendingBytes += wireBytes;
            found->second.PendingClosed =
                notification.Kind == ControlNotificationKind::SubscriptionClosed;
            return RuntimeControlNotificationStatus::Delivered;
        }

        if (notification.Sequence <= found->second.Cursor)
        {
            return RuntimeControlNotificationStatus::Duplicate;
        }
        if (found->second.Cursor == std::numeric_limits<uint64_t>::max() ||
            notification.Sequence != found->second.Cursor + 1)
        {
            return RuntimeControlNotificationStatus::SequenceGap;
        }

        const auto queued = outbound.Enqueue(std::move(encoded.Payload));
        if (queued == RuntimeControlQueueStatus::Accepted)
        {
            found->second.Cursor = notification.Sequence;
            if (notification.Kind == ControlNotificationKind::SubscriptionClosed)
            {
                _impl->Subscriptions.erase(found);
            }
            return RuntimeControlNotificationStatus::Delivered;
        }
        return queued == RuntimeControlQueueStatus::QueueLimitReached ?
                   RuntimeControlNotificationStatus::QueueLimitReached :
               queued == RuntimeControlQueueStatus::Closed ?
                   RuntimeControlNotificationStatus::Closed :
                   RuntimeControlNotificationStatus::InvalidNotification;
    }

    void RuntimeControlSubscriptions::ClearAndClose(RuntimeControlQueue& outbound) noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        _impl->Subscriptions.clear();
        _impl->PendingMessages = 0;
        _impl->PendingBytes = 0;
        outbound.Close();
    }
}
