// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

#include "SnapshotAndSubscribe.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        bool _activeLocked(
            const RuntimeClientSharedState& state,
            const std::shared_ptr<RuntimeClientEpochContext>& epoch) noexcept
        {
            return epoch && state.Active && state.Active.get() == epoch.get() &&
                   state.Epochs.IsActive(epoch->Epoch);
        }

        RuntimeClientLogicalOperationKind _operationKind(
            const RuntimeClientSubscriptionRequestKind kind) noexcept
        {
            return kind == RuntimeClientSubscriptionRequestKind::Snapshot ?
                       RuntimeClientLogicalOperationKind::SubscriptionSnapshot :
                       RuntimeClientLogicalOperationKind::SubscriptionUnsubscribe;
        }

        ControlMethod _method(const RuntimeClientSubscriptionRequestKind kind) noexcept
        {
            return kind == RuntimeClientSubscriptionRequestKind::Snapshot ?
                       ControlMethod::SnapshotAndSubscribe :
                       ControlMethod::Unsubscribe;
        }

        bool _matches(
            const RuntimeClientLogicalOperation& operation,
            const RuntimeClientRequestAttemptInfo& attempt,
            const RuntimeClientSubscriptionRequestBinding& binding,
            const std::string_view requestId) noexcept
        {
            const auto method = _method(binding.Kind);
            return operation.OperationId == attempt.OperationId &&
                   operation.Kind == _operationKind(binding.Kind) &&
                   operation.Subscription.LogicalId == binding.LogicalId &&
                   operation.Subscription.Revision == binding.Revision &&
                   operation.Method == method && attempt.Method == method &&
                   operation.CurrentAttempt == attempt.Attempt &&
                   operation.CurrentRequestId == requestId && attempt.RequestId == requestId;
        }

        bool _retryableSnapshotError(const ProtocolRpcErrorCode code) noexcept
        {
            return code == ProtocolRpcErrorCode::RuntimeUnavailable ||
                   code == ProtocolRpcErrorCode::InternalError;
        }

        bool _capacityFailure(const RuntimeClientSubscriptionPublishStatus status) noexcept
        {
            return status == RuntimeClientSubscriptionPublishStatus::EventTooLarge ||
                   status == RuntimeClientSubscriptionPublishStatus::CountLimitReached ||
                   status == RuntimeClientSubscriptionPublishStatus::ByteLimitReached ||
                   status == RuntimeClientSubscriptionPublishStatus::AggregateLimitReached ||
                   status == RuntimeClientSubscriptionPublishStatus::AllocationFailure;
        }

        RuntimeClientPendingSubscriptionDelivery _delivery(
            std::shared_ptr<RuntimeClientSubscriptionStreamProducer> producer,
            const RuntimeClientSubscriptionTarget target,
            const RuntimeClientEpoch epoch,
            const RuntimeClientSubscriptionEventKind kind,
            const bool close) noexcept
        {
            RuntimeClientPendingSubscriptionDelivery result;
            result.Producer = std::move(producer);
            result.Event.Kind = kind;
            result.Event.LogicalId = target.LogicalId;
            result.Event.Revision = target.Revision;
            result.Event.Epoch = epoch;
            result.Close = close;
            return result;
        }

        void _stage(
            std::optional<RuntimeClientPendingSubscriptionDelivery>& delivery,
            const std::shared_ptr<RuntimeClientSubscriptionStreamProducer>& producer,
            const RuntimeClientSubscriptionTarget target,
            const RuntimeClientEpoch epoch,
            const RuntimeClientSubscriptionEventKind kind,
            const bool close) noexcept
        {
            delivery.emplace(_delivery(producer, target, epoch, kind, close));
        }

        RuntimeClientSubscriptionPublishStatus _deliver(
            RuntimeClientPendingSubscriptionDelivery delivery) noexcept
        {
            if (!delivery.Producer)
            {
                return RuntimeClientSubscriptionPublishStatus::InvalidEvent;
            }
            return delivery.Close ?
                       delivery.Producer->Close(std::move(delivery.Event)) :
                       delivery.Producer->Publish(std::move(delivery.Event));
        }

        void _protocolRetire(
            RuntimeClientSharedState& state,
            const std::shared_ptr<RuntimeClientEpochContext>& epoch,
            const std::string_view diagnostic) noexcept
        {
            state.RequestEpochRetire(
                epoch,
                RuntimeClientRetireCause::ProtocolError,
                RuntimeTransportStatus::MalformedFrame,
                ERROR_INVALID_DATA,
                diagnostic);
        }

        void _resnapshotRetire(
            RuntimeClientSharedState& state,
            const std::shared_ptr<RuntimeClientEpochContext>& epoch,
            const DWORD systemError,
            const std::string_view diagnostic) noexcept
        {
            state.RequestEpochRetire(
                epoch,
                RuntimeClientRetireCause::TransportFailure,
                RuntimeTransportStatus::Disconnected,
                systemError,
                diagnostic);
        }

        void _recoverPublishCapacity(
            RuntimeClientSharedState& state,
            const std::shared_ptr<RuntimeClientEpochContext>& epoch,
            const RuntimeClientSubscriptionTarget target,
            const std::shared_ptr<RuntimeClientSubscriptionStreamProducer>& producer,
            const bool alreadyNeedsSnapshot) noexcept
        {
            std::optional<RuntimeClientPendingSubscriptionDelivery> removed;
            bool retire{};
            bool protocolFault{};
            try
            {
                {
                    std::scoped_lock lock{ state.Mutex };
                    if (!_activeLocked(state, epoch))
                    {
                        return;
                    }
                    const auto descriptor = state.SubscriptionDescriptors.find(target.LogicalId);
                    if (descriptor == state.SubscriptionDescriptors.end())
                    {
                        return;
                    }
                    if (descriptor->second.Revision != target.Revision ||
                        descriptor->second.Producer != producer)
                    {
                        protocolFault = true;
                    }
                    else
                    {
                        const auto forced = state.Subscriptions.ForceNeedsSnapshot(
                            target.LogicalId,
                            target.Revision,
                            epoch->Epoch);
                        if (forced == RuntimeClientSubscriptionStatus::Succeeded ||
                            (alreadyNeedsSnapshot && forced == RuntimeClientSubscriptionStatus::Stale))
                        {
                            retire = true;
                        }
                        else if (forced == RuntimeClientSubscriptionStatus::Finalized)
                        {
                            _stage(removed, descriptor->second.Producer, target, epoch->Epoch,
                                   RuntimeClientSubscriptionEventKind::Removed, true);
                            state.SubscriptionDescriptors.erase(descriptor);
                            retire = true;
                        }
                        else
                        {
                            protocolFault = true;
                        }
                    }
                }
                if (removed)
                {
                    (void)_deliver(std::move(*removed));
                }
            }
            catch (...)
            {
                protocolFault = true;
            }

            if (protocolFault)
            {
                _protocolRetire(state, epoch, "Subscription delivery overflow recovery was inconsistent");
            }
            else if (retire)
            {
                _resnapshotRetire(
                    state,
                    epoch,
                    ERROR_NOT_ENOUGH_MEMORY,
                    "Subscription delivery capacity requires a clean resnapshot");
            }
        }

        void _publish(
            RuntimeClientSharedState& state,
            const std::shared_ptr<RuntimeClientEpochContext>& epoch,
            RuntimeClientPendingSubscriptionDelivery delivery,
            const bool retireForResnapshot) noexcept
        {
            const auto target = RuntimeClientSubscriptionTarget{
                delivery.Event.LogicalId,
                delivery.Event.Revision,
            };
            const auto producer = delivery.Producer;
            const auto deliveredSnapshot =
                delivery.Event.Kind == RuntimeClientSubscriptionEventKind::Snapshot;
            const auto status = _deliver(std::move(delivery));
            if (status == RuntimeClientSubscriptionPublishStatus::Accepted && deliveredSnapshot)
            {
                std::scoped_lock lock{ state.Mutex };
                const auto descriptor = state.SubscriptionDescriptors.find(target.LogicalId);
                if (descriptor != state.SubscriptionDescriptors.end() &&
                    descriptor->second.Revision == target.Revision &&
                    descriptor->second.Producer == producer)
                {
                    descriptor->second.SnapshotAttempts = 0;
                }
            }
            if (_capacityFailure(status))
            {
                _recoverPublishCapacity(
                    state,
                    epoch,
                    target,
                    producer,
                    retireForResnapshot);
                return;
            }
            if (status == RuntimeClientSubscriptionPublishStatus::InvalidEvent)
            {
                _protocolRetire(state, epoch, "Subscription delivery event was invalid");
                return;
            }
            if (retireForResnapshot)
            {
                _resnapshotRetire(
                    state,
                    epoch,
                    ERROR_INVALID_DATA,
                    "Subscription cursor requires a clean resnapshot");
            }
        }
    }

    // Internal cross-translation-unit seam used by notification handling. It
    // is deliberately absent from the public client headers.
    void PublishRuntimeClientSubscriptionDelivery(
        RuntimeClientSharedState& state,
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        RuntimeClientPendingSubscriptionDelivery delivery,
        const bool retireForResnapshot) noexcept
    {
        _publish(state, epoch, std::move(delivery), retireForResnapshot);
    }

    bool RuntimeClientSharedState::HandleSubscriptionResponse(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        ControlResponse& response) noexcept
    {
        std::optional<RuntimeClientSubscriptionRequestBinding> binding;
        try
        {
            {
                std::scoped_lock lock{ Mutex };
                if (!_activeLocked(*this, epoch))
                {
                    return true;
                }
                binding = Subscriptions.FindRequestBinding(epoch->Epoch, response.RequestId);
            }
            if (!binding)
            {
                return false;
            }

            ProtocolRpcJsonDecodeResult<SnapshotAndSubscribeResult> snapshot;
            const auto decodeSnapshot = binding->Kind == RuntimeClientSubscriptionRequestKind::Snapshot &&
                                        response.Succeeded;
            if (decodeSnapshot)
            {
                snapshot = DecodeSnapshotAndSubscribeResult(response.ResultJson);
            }

            std::optional<RuntimeClientPendingSubscriptionDelivery> delivery;
            bool protocolFault{};
            bool retireForResnapshot{};
            bool wakeWriter{};
            {
                std::scoped_lock lock{ Mutex };
                if (!_activeLocked(*this, epoch))
                {
                    return true;
                }
                const auto correlated = Requests.CompleteResponse(
                    epoch->Epoch,
                    Active->Epoch,
                    response.RequestId);
                if (correlated.Status != RuntimeClientResponseMatch::Matched || !correlated.Attempt)
                {
                    protocolFault = true;
                }
                else
                {
                    const auto operation = Operations.find(correlated.Attempt->OperationId);
                    if (operation == Operations.end() ||
                        !_matches(operation->second, *correlated.Attempt, *binding, response.RequestId))
                    {
                        protocolFault = true;
                    }
                    else
                    {
                        const auto operationId = operation->first;
                        const auto target = operation->second.Subscription;
                        const auto descriptor = SubscriptionDescriptors.find(target.LogicalId);
                        if (descriptor == SubscriptionDescriptors.end() ||
                            descriptor->second.Revision != target.Revision || !descriptor->second.Producer)
                        {
                            protocolFault = true;
                        }
                        else if (binding->Kind == RuntimeClientSubscriptionRequestKind::Snapshot)
                        {
                            if (response.Succeeded)
                            {
                                if (!snapshot.Succeeded())
                                {
                                    protocolFault = true;
                                }
                                else
                                {
                                    const auto applied = Subscriptions.ApplySnapshot(
                                        epoch->Epoch,
                                        response.RequestId,
                                        snapshot.Value);
                                    if (applied.Status != RuntimeClientSubscriptionStatus::Succeeded ||
                                        applied.LogicalId != target.LogicalId ||
                                        applied.Revision != target.Revision)
                                    {
                                        protocolFault = true;
                                    }
                                    else
                                    {
                                        _stage(delivery, descriptor->second.Producer, target, epoch->Epoch,
                                               RuntimeClientSubscriptionEventKind::Snapshot, false);
                                        delivery->Event.Cursor = snapshot.Value.Cursor;
                                        delivery->Event.SnapshotJson = std::move(snapshot.Value.SnapshotJson);
                                        wakeWriter = applied.NeedsUnsubscribe;
                                    }
                                }
                            }
                            else if (!response.Error)
                            {
                                protocolFault = true;
                            }
                            else
                            {
                                const auto failed = Subscriptions.FailRequest(
                                    epoch->Epoch,
                                    response.RequestId);
                                if (failed == RuntimeClientSubscriptionStatus::Finalized)
                                {
                                    _stage(delivery, descriptor->second.Producer, target, epoch->Epoch,
                                           RuntimeClientSubscriptionEventKind::Removed, true);
                                    SubscriptionDescriptors.erase(descriptor);
                                }
                                else if (failed != RuntimeClientSubscriptionStatus::Succeeded)
                                {
                                    protocolFault = true;
                                }
                                else if (_retryableSnapshotError(response.Error->Code) &&
                                         descriptor->second.SnapshotAttempts <
                                             Configuration.MaximumSubscriptionAttempts)
                                {
                                    retireForResnapshot = true;
                                }
                                else
                                {
                                    const auto removed = Subscriptions.BeginRemove(
                                        target.LogicalId,
                                        target.Revision);
                                    if (removed.Status != RuntimeClientSubscriptionStatus::Finalized)
                                    {
                                        protocolFault = true;
                                    }
                                    else
                                    {
                                        _stage(delivery, descriptor->second.Producer, target, epoch->Epoch,
                                               RuntimeClientSubscriptionEventKind::Faulted, true);
                                        delivery->Event.TransportStatus = RuntimeTransportStatus::ProtocolRejected;
                                        delivery->Event.SystemError = ERROR_GEN_FAILURE;
                                        delivery->Event.Diagnostic = std::move(response.Error->Message);
                                        SubscriptionDescriptors.erase(descriptor);
                                    }
                                }
                            }
                        }
                        else if (response.Succeeded)
                        {
                            const auto completed = Subscriptions.CompleteUnsubscribe(
                                epoch->Epoch,
                                response.RequestId);
                            if (completed != RuntimeClientSubscriptionStatus::Finalized)
                            {
                                protocolFault = true;
                            }
                            else
                            {
                                _stage(delivery, descriptor->second.Producer, target, epoch->Epoch,
                                       RuntimeClientSubscriptionEventKind::Removed, true);
                                SubscriptionDescriptors.erase(descriptor);
                            }
                        }
                        else if (!response.Error ||
                                 Subscriptions.FailRequest(epoch->Epoch, response.RequestId) !=
                                     RuntimeClientSubscriptionStatus::Succeeded)
                        {
                            protocolFault = true;
                        }
                        else
                        {
                            retireForResnapshot = true;
                        }

                        EraseSubscriptionOperationLocked(operationId);
                        ConsecutiveFailures = 0;
                    }
                }
            }

            if (wakeWriter)
            {
                WriterWake.notify_all();
            }
            if (delivery)
            {
                if (delivery->Close)
                {
                    (void)_deliver(std::move(*delivery));
                }
                else
                {
                    _publish(*this, epoch, std::move(*delivery), false);
                }
            }
            if (protocolFault)
            {
                _protocolRetire(*this, epoch, "Subscription response correlation or state was invalid");
            }
            else if (retireForResnapshot)
            {
                _resnapshotRetire(
                    *this,
                    epoch,
                    ERROR_RETRY,
                    "Subscription RPC failure requires a clean resnapshot");
            }
            return true;
        }
        catch (...)
        {
            _protocolRetire(*this, epoch, "Unexpected subscription response handling failure");
            return true;
        }
    }

}
