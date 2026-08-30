// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

#include "ProtocolRpcJson.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime::details
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        constexpr std::string_view ValidationRequestId{
            "30000000-0000-4000-8000-000000000202"
        };

        RuntimeClientSubscriptionEvent _terminalEvent(
            const RuntimeClientLogicalSubscriptionId logicalId,
            const RuntimeClientSubscriptionRevision revision,
            const RuntimeClientSubscriptionEventKind kind,
            const RuntimeTransportStatus transportStatus = RuntimeTransportStatus::Succeeded,
            const DWORD systemError = ERROR_SUCCESS,
            const std::string_view diagnostic = {})
        {
            RuntimeClientSubscriptionEvent event;
            event.Kind = kind;
            event.LogicalId = logicalId;
            event.Revision = revision;
            event.TransportStatus = transportStatus;
            event.SystemError = systemError;
            try
            {
                const auto length = (std::min)(
                    diagnostic.size(),
                    MaximumSubscriptionEventDiagnosticBytes);
                event.Diagnostic.assign(diagnostic.data(), length);
            }
            catch (...)
            {
                event.Diagnostic.clear();
            }
            return event;
        }
    }

    RuntimeClientSubscriptionHandle RuntimeClientSharedState::Subscribe(
        RuntimeClientSubscribeOptions options)
    {
        RuntimeClientSubscriptionHandle rejected;
        ControlRequest validation;
        validation.RequestId = ValidationRequestId;
        validation.Method = ControlMethod::SnapshotAndSubscribe;
        validation.PayloadJson = options.ParamsJson;
        const auto encoded = EncodeControlRequest(validation);
        if (!encoded.Succeeded || encoded.Payload.size() > Configuration.MaximumRequestBytes)
        {
            return rejected;
        }

        RuntimeClientLogicalSubscriptionId logicalId{};
        {
            std::scoped_lock lock{ Mutex };
            if (!AdmissionOpen || SubscriptionDescriptors.size() >= Configuration.MaximumSubscriptions ||
                NextSubscriptionId == 0)
            {
                return rejected;
            }
            logicalId = NextSubscriptionId;
            NextSubscriptionId = NextSubscriptionId ==
                                         (std::numeric_limits<RuntimeClientLogicalSubscriptionId>::max)() ?
                                     0 :
                                     NextSubscriptionId + 1;
        }

        constexpr RuntimeClientSubscriptionRevision revision{ 1 };
        std::string paramsJson;
        try
        {
            paramsJson = options.ParamsJson;
        }
        catch (...)
        {
            return rejected;
        }

        std::weak_ptr<RuntimeClientSharedState> weakState;
        try
        {
            weakState = shared_from_this();
        }
        catch (...)
        {
            return rejected;
        }
        auto endpoints = RuntimeClientSubscriptionStreamFactory::Create(
            logicalId,
            revision,
            std::move(options),
            [weakState](const auto id, const auto expectedRevision) noexcept {
                if (const auto state = weakState.lock())
                {
                    state->RequestSubscriptionRemove(id, expectedRevision);
                }
            },
            SubscriptionBudget);
        if (endpoints.Status != RuntimeClientSubscriptionStreamCreateStatus::Succeeded ||
            !endpoints.Stream || !endpoints.Producer)
        {
            return rejected;
        }

        bool accepted{};
        {
            std::scoped_lock lock{ Mutex };
            if (AdmissionOpen && SubscriptionDescriptors.size() < Configuration.MaximumSubscriptions &&
                Subscriptions.Add(logicalId, revision) == RuntimeClientSubscriptionStatus::Succeeded)
            {
                try
                {
                    RuntimeClientSubscriptionDescriptor descriptor;
                    descriptor.Revision = revision;
                    descriptor.ParamsJson = std::move(paramsJson);
                    descriptor.Producer = endpoints.Producer;
                    accepted = SubscriptionDescriptors.emplace(logicalId, std::move(descriptor)).second;
                }
                catch (...)
                {
                    accepted = false;
                }
                if (!accepted)
                {
                    (void)Subscriptions.BeginRemove(logicalId, revision);
                }
            }
        }
        if (!accepted)
        {
            return rejected;
        }

        WriterWake.notify_all();
        return { true, logicalId, revision, std::move(endpoints.Stream) };
    }

    void RuntimeClientSharedState::RequestSubscriptionRemove(
        const RuntimeClientLogicalSubscriptionId logicalId,
        const RuntimeClientSubscriptionRevision revision) noexcept
    {
        std::shared_ptr<RuntimeClientSubscriptionStreamProducer> producer;
        bool finalized{};
        bool wake{};
        try
        {
            {
                std::scoped_lock lock{ Mutex };
                const auto descriptor = SubscriptionDescriptors.find(logicalId);
                if (descriptor == SubscriptionDescriptors.end() || descriptor->second.Revision != revision)
                {
                    return;
                }
                const auto removal = Subscriptions.BeginRemoveForIntegration(logicalId, revision);
                finalized = removal.Status == RuntimeClientSubscriptionStatus::Finalized;
                wake = removal.Status == RuntimeClientSubscriptionStatus::Succeeded;
                if (finalized)
                {
                    producer = descriptor->second.Producer;
                    SubscriptionDescriptors.erase(descriptor);
                }
            }
            if (producer)
            {
                (void)producer->Close(_terminalEvent(
                    logicalId,
                    revision,
                    RuntimeClientSubscriptionEventKind::Removed));
            }
            if (wake)
            {
                WriterWake.notify_all();
            }
        }
        catch (...)
        {
        }
    }

    RuntimeClientSubscriptionWriteResult RuntimeClientSharedState::RejectOversizedSubscriptionRequest(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        const RuntimeClientSubscriptionTarget target) noexcept
    {
        std::optional<RuntimeClientPendingSubscriptionDelivery> delivery;
        bool inconsistent{};
        try
        {
            {
                std::scoped_lock lock{ Mutex };
                if (ClientState == TaskRuntimeClientState::Stopping || !epoch || !Active ||
                    Active.get() != epoch.get() || !Epochs.IsActive(epoch->Epoch))
                {
                    return RuntimeClientSubscriptionWriteResult::Stop;
                }

                const auto descriptor = SubscriptionDescriptors.find(target.LogicalId);
                if (descriptor == SubscriptionDescriptors.end())
                {
                    return RuntimeClientSubscriptionWriteResult::NoWork;
                }
                const auto view = Subscriptions.Find(target.LogicalId);
                if (!view || descriptor->second.Revision != target.Revision ||
                    view->Revision != target.Revision)
                {
                    inconsistent = true;
                }
                else if (view->Phase != RuntimeClientSubscriptionPhase::Desired &&
                         view->Phase != RuntimeClientSubscriptionPhase::NeedsSnapshot)
                {
                    return RuntimeClientSubscriptionWriteResult::NoWork;
                }
                else
                {
                    const auto removed = Subscriptions.BeginRemoveForIntegration(
                        target.LogicalId,
                        target.Revision);
                    if (removed.Status != RuntimeClientSubscriptionStatus::Finalized)
                    {
                        inconsistent = true;
                    }
                    else
                    {
                        RuntimeClientPendingSubscriptionDelivery rejected;
                        rejected.Producer = descriptor->second.Producer;
                        rejected.Event = _terminalEvent(
                            target.LogicalId,
                            target.Revision,
                            RuntimeClientSubscriptionEventKind::Faulted,
                            RuntimeTransportStatus::ProtocolRejected,
                            ERROR_BAD_LENGTH,
                            "Subscription request exceeds the negotiated control payload limit");
                        rejected.Event.Epoch = epoch->Epoch;
                        rejected.Close = true;
                        delivery.emplace(std::move(rejected));
                        SubscriptionDescriptors.erase(descriptor);
                    }
                }
            }
        }
        catch (...)
        {
            inconsistent = true;
        }

        if (delivery && delivery->Producer)
        {
            (void)delivery->Producer->Close(std::move(delivery->Event));
        }
        if (inconsistent)
        {
            RequestEpochRetire(
                epoch,
                RuntimeClientRetireCause::ProtocolError,
                RuntimeTransportStatus::SystemError,
                ERROR_INVALID_STATE,
                "Oversized subscription rejection state was inconsistent");
            return RuntimeClientSubscriptionWriteResult::RetireEpoch;
        }
        return RuntimeClientSubscriptionWriteResult::NoWork;
    }

    void RuntimeClientSharedState::RetireSubscriptionsLocked(
        const RuntimeClientEpoch epoch,
        const bool stopping,
        std::vector<RuntimeClientPendingSubscriptionDelivery>& deliveries) noexcept
    {
        (void)Subscriptions.RetireEpoch(epoch);
        if (stopping)
        {
            return;
        }
        auto current = SubscriptionDescriptors.begin();
        while (current != SubscriptionDescriptors.end())
        {
            std::optional<RuntimeClientSubscriptionView> view;
            try
            {
                view = Subscriptions.Find(current->first);
            }
            catch (...)
            {
                ++current;
                continue;
            }
            if (view)
            {
                if (view->Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot &&
                    current->second.SnapshotAttempts >= Configuration.MaximumSubscriptionAttempts)
                {
                    const auto removed = Subscriptions.BeginRemove(
                        current->first,
                        current->second.Revision);
                    if (removed.Status == RuntimeClientSubscriptionStatus::Finalized)
                    {
                        RuntimeClientPendingSubscriptionDelivery delivery;
                        delivery.Producer = current->second.Producer;
                        delivery.Event = _terminalEvent(
                            current->first,
                            current->second.Revision,
                            RuntimeClientSubscriptionEventKind::Faulted,
                            RuntimeTransportStatus::Disconnected,
                            ERROR_RETRY,
                            "Subscription snapshot attempt limit was exhausted");
                        delivery.Close = true;
                        deliveries.emplace_back(std::move(delivery));
                        current = SubscriptionDescriptors.erase(current);
                        continue;
                    }
                }
                ++current;
                continue;
            }
            RuntimeClientPendingSubscriptionDelivery delivery;
            delivery.Producer = current->second.Producer;
            delivery.Event = _terminalEvent(
                current->first,
                current->second.Revision,
                RuntimeClientSubscriptionEventKind::Removed);
            delivery.Close = true;
            deliveries.emplace_back(std::move(delivery));
            current = SubscriptionDescriptors.erase(current);
        }
    }

    void RuntimeClientSharedState::FailSubscriptionsLocked(
        const RuntimeClientSubscriptionEventKind kind,
        const RuntimeTransportStatus transportStatus,
        const DWORD systemError,
        const std::string_view diagnostic,
        std::vector<RuntimeClientPendingSubscriptionDelivery>& deliveries) noexcept
    {
        for (const auto& [logicalId, descriptor] : SubscriptionDescriptors)
        {
            RuntimeClientPendingSubscriptionDelivery delivery;
            delivery.Producer = descriptor.Producer;
            delivery.Event = _terminalEvent(
                logicalId,
                descriptor.Revision,
                kind,
                transportStatus,
                systemError,
                diagnostic);
            delivery.Close = true;
            deliveries.emplace_back(std::move(delivery));
        }
        SubscriptionDescriptors.clear();
    }

    void RuntimeClientSharedState::DeliverSubscriptions(
        std::vector<RuntimeClientPendingSubscriptionDelivery>& deliveries) noexcept
    {
        for (auto& delivery : deliveries)
        {
            if (!delivery.Producer)
            {
                continue;
            }
            if (delivery.Close)
            {
                (void)delivery.Producer->Close(std::move(delivery.Event));
            }
            else
            {
                (void)delivery.Producer->Publish(std::move(delivery.Event));
            }
        }
        deliveries.clear();
    }
}
