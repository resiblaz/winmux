// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlPublisher.h"

#include "RuntimeControlSession.h"

#include "ProtocolRpcJson.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    RuntimeControlPublishStatus PublishRuntimeControlNotification(
        const ControlNotification& notification,
        std::mutex& sessionsMutex,
        const std::vector<std::unique_ptr<RuntimeControlSession>>& sessions,
        std::mutex& subscriptionOwnershipMutex,
        const std::atomic_bool& stopRequested,
        const std::atomic_bool& isRunning)
    {
        if (!EncodeControlNotification(notification).Succeeded)
        {
            return RuntimeControlPublishStatus::InvalidNotification;
        }

        std::unique_lock sessionsLock{ sessionsMutex };
        if (stopRequested.load(std::memory_order_acquire) ||
            !isRunning.load(std::memory_order_acquire))
        {
            return RuntimeControlPublishStatus::ServerStopped;
        }
        std::unique_lock ownershipLock{ subscriptionOwnershipMutex };
        RuntimeControlSession* target{};
        for (const auto& session : sessions)
        {
            if (session->OwnsSubscription(
                    notification.SubscriptionId,
                    notification.Generation))
            {
                if (target)
                {
                    return RuntimeControlPublishStatus::AmbiguousSubscription;
                }
                target = session.get();
            }
        }
        if (!target)
        {
            return RuntimeControlPublishStatus::NoSubscriber;
        }

        const auto published = target->PublishNotification(notification);
        ownershipLock.unlock();
        if (published == RuntimeControlNotificationStatus::QueueLimitReached)
        {
            target->RequestStop(RuntimeControlSessionCloseReason::OutboundQueueLimit);
        }
        switch (published)
        {
        case RuntimeControlNotificationStatus::Delivered:
            return RuntimeControlPublishStatus::Delivered;
        case RuntimeControlNotificationStatus::Duplicate:
            return RuntimeControlPublishStatus::Duplicate;
        case RuntimeControlNotificationStatus::SequenceGap:
            return RuntimeControlPublishStatus::SequenceGap;
        case RuntimeControlNotificationStatus::QueueLimitReached:
            return RuntimeControlPublishStatus::SlowClientDisconnected;
        case RuntimeControlNotificationStatus::InvalidNotification:
            return RuntimeControlPublishStatus::InvalidNotification;
        case RuntimeControlNotificationStatus::Closed:
        case RuntimeControlNotificationStatus::NotSubscribed:
        default:
            return RuntimeControlPublishStatus::NoSubscriber;
        }
    }
}
