// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    void PublishRuntimeClientSubscriptionDelivery(
        RuntimeClientSharedState& state,
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        RuntimeClientPendingSubscriptionDelivery delivery,
        bool retireForResnapshot) noexcept;

    namespace
    {
        bool _activeLocked(
            const RuntimeClientSharedState& state,
            const std::shared_ptr<RuntimeClientEpochContext>& epoch) noexcept
        {
            return epoch && state.Active && state.Active.get() == epoch.get() &&
                   state.Epochs.IsActive(epoch->Epoch);
        }

        RuntimeClientPendingSubscriptionDelivery _delivery(
            const std::shared_ptr<RuntimeClientSubscriptionStreamProducer>& producer,
            const RuntimeClientSubscriptionTarget target,
            const RuntimeClientEpoch epoch,
            const RuntimeClientSubscriptionEventKind kind,
            const bool close) noexcept
        {
            RuntimeClientPendingSubscriptionDelivery result;
            result.Producer = producer;
            result.Event.Kind = kind;
            result.Event.LogicalId = target.LogicalId;
            result.Event.Revision = target.Revision;
            result.Event.Epoch = epoch;
            result.Close = close;
            return result;
        }

        void _resnapshotRetire(
            RuntimeClientSharedState& state,
            const std::shared_ptr<RuntimeClientEpochContext>& epoch,
            const std::string_view diagnostic) noexcept
        {
            state.RequestEpochRetire(
                epoch,
                RuntimeClientRetireCause::TransportFailure,
                RuntimeTransportStatus::Disconnected,
                ERROR_INVALID_DATA,
                diagnostic);
        }
    }

    bool RuntimeClientSharedState::HandleSubscriptionNotification(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        ControlNotification notification,
        const RuntimeClientMonotonicMilliseconds now) noexcept
    {
        try
        {
            std::optional<RuntimeClientPendingSubscriptionDelivery> delivery;
            bool retireForResnapshot{};
            RuntimeClientHeartbeatStatus heartbeatStatus{ RuntimeClientHeartbeatStatus::Stale };
            {
                std::scoped_lock lock{ Mutex };
                if (!_activeLocked(*this, epoch))
                {
                    return true;
                }
                const auto observed = Subscriptions.Observe(epoch->Epoch, notification);
                if (observed.Status != RuntimeClientSubscriptionStatus::Succeeded)
                {
                    return false;
                }
                const auto descriptor = SubscriptionDescriptors.find(observed.LogicalId);
                if (descriptor == SubscriptionDescriptors.end() ||
                    descriptor->second.Revision != observed.Revision || !descriptor->second.Producer)
                {
                    return false;
                }

                const auto target = RuntimeClientSubscriptionTarget{
                    observed.LogicalId,
                    observed.Revision,
                };
                const auto isSubscriptionClosed =
                    notification.Kind == ControlNotificationKind::SubscriptionClosed;
                const auto closed = observed.Disposition == NotificationDisposition::Apply &&
                                    isSubscriptionClosed;
                const auto resnapshot = observed.Disposition == NotificationDisposition::Resnapshot || closed;
                const auto refreshHeartbeat =
                    !isSubscriptionClosed &&
                    (observed.Disposition == NotificationDisposition::IgnoreDuplicate ||
                     observed.Disposition == NotificationDisposition::IgnoreUnknown ||
                     observed.Disposition == NotificationDisposition::Apply);
                if (refreshHeartbeat)
                {
                    heartbeatStatus = ObserveInboundLocked(epoch, now);
                }
                if (observed.Finalized)
                {
                    delivery.emplace(_delivery(
                        descriptor->second.Producer,
                        target,
                        epoch->Epoch,
                        RuntimeClientSubscriptionEventKind::Removed,
                        true));
                    SubscriptionDescriptors.erase(descriptor);
                    retireForResnapshot = resnapshot;
                }
                else if (resnapshot)
                {
                    delivery.emplace(_delivery(
                        descriptor->second.Producer,
                        target,
                        epoch->Epoch,
                        RuntimeClientSubscriptionEventKind::ResnapshotRequired,
                        false));
                    retireForResnapshot = true;
                }
                else if (observed.Disposition == NotificationDisposition::Apply && observed.Deliver)
                {
                    delivery.emplace(_delivery(
                        descriptor->second.Producer,
                        target,
                        epoch->Epoch,
                        RuntimeClientSubscriptionEventKind::Notification,
                        false));
                    delivery->Event.Cursor = notification.Sequence;
                    delivery->Event.Notification = std::move(notification);
                }
                else if (observed.Disposition != NotificationDisposition::IgnoreDuplicate &&
                         observed.Disposition != NotificationDisposition::IgnoreUnknown &&
                         observed.Disposition != NotificationDisposition::Apply)
                {
                    return false;
                }
            }

            (void)RetireForHeartbeatObservation(
                epoch,
                heartbeatStatus,
                "Valid subscription notification arrived at or after the heartbeat silence deadline");

            if (delivery)
            {
                if (delivery->Close)
                {
                    if (delivery->Producer)
                    {
                        (void)delivery->Producer->Close(std::move(delivery->Event));
                    }
                    if (retireForResnapshot)
                    {
                        _resnapshotRetire(
                            *this,
                            epoch,
                            "Closed subscription requires connection retirement");
                    }
                }
                else
                {
                    PublishRuntimeClientSubscriptionDelivery(
                        *this,
                        epoch,
                        std::move(*delivery),
                        retireForResnapshot);
                }
            }
            else if (retireForResnapshot)
            {
                _resnapshotRetire(
                    *this,
                    epoch,
                    "Subscription cursor requires a clean resnapshot");
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
}
