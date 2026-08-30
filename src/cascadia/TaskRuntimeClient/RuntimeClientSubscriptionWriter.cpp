// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

#include "ProtocolRpcJson.h"
#include "ProtocolValidation.h"
#include "SnapshotAndSubscribe.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        enum class SubscriptionWorkKind
        {
            Snapshot,
            Unsubscribe,
        };

        struct SubscriptionWork
        {
            SubscriptionWorkKind Kind{ SubscriptionWorkKind::Snapshot };
            RuntimeClientSubscriptionTarget Target;
            RuntimeClientAttempt Attempt{ 1 };
            std::string ParamsJson;
            UnsubscribeRequest Unsubscribe;
        };

        bool _unsubscribeReady(
            const RuntimeClientSubscriptionView& view,
            const RuntimeClientEpoch epoch) noexcept
        {
            return view.Phase == RuntimeClientSubscriptionPhase::Removing &&
                   view.Epoch == epoch && view.RequestId.empty() &&
                   !view.WireSubscriptionId.empty() && view.Generation != 0;
        }

        bool _internalOperation(const RuntimeClientLogicalOperationKind kind) noexcept
        {
            return kind == RuntimeClientLogicalOperationKind::SubscriptionSnapshot ||
                   kind == RuntimeClientLogicalOperationKind::SubscriptionUnsubscribe;
        }
    }

    bool RuntimeClientSharedState::HasSubscriptionWriterWorkLocked(
        const RuntimeClientEpoch epoch) const noexcept
    {
        if (epoch == 0 || SubscriptionOperationCount >=
                              Configuration.MaximumConcurrentSubscriptionOperations)
        {
            return false;
        }

        try
        {
            for (const auto& [logicalId, descriptor] : SubscriptionDescriptors)
            {
                const auto view = Subscriptions.Find(logicalId);
                if (view && view->Revision == descriptor.Revision &&
                    _unsubscribeReady(*view, epoch))
                {
                    return true;
                }
            }

            if (const auto target = Subscriptions.NextNeedingSnapshot())
            {
                const auto descriptor = SubscriptionDescriptors.find(target->LogicalId);
                return descriptor == SubscriptionDescriptors.end() ||
                       descriptor->second.Revision != target->Revision ||
                       descriptor->second.SnapshotAttempts <
                           Configuration.MaximumSubscriptionAttempts;
            }
        }
        catch (...)
        {
            // Let RunSubscriptionWriter turn an allocation failure while
            // inspecting state into an epoch retirement instead of sleeping.
            return true;
        }
        return false;
    }

    RuntimeClientSubscriptionWriteResult RuntimeClientSharedState::RunSubscriptionWriter(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch) noexcept
    {
        if (!epoch)
        {
            return RuntimeClientSubscriptionWriteResult::Stop;
        }

        const auto retireProtocol = [&](const DWORD error, const std::string_view diagnostic) noexcept {
            RequestEpochRetire(
                epoch,
                RuntimeClientRetireCause::ProtocolError,
                RuntimeTransportStatus::SystemError,
                error,
                diagnostic);
            return RuntimeClientSubscriptionWriteResult::RetireEpoch;
        };

        try
        {
            std::optional<SubscriptionWork> selected;
            bool inconsistent{};
            {
                std::scoped_lock lock{ Mutex };
                if (ClientState == TaskRuntimeClientState::Stopping ||
                    !Active || Active.get() != epoch.get() ||
                    !Epochs.IsActive(epoch->Epoch))
                {
                    return RuntimeClientSubscriptionWriteResult::Stop;
                }
                if (SubscriptionOperationCount >=
                    Configuration.MaximumConcurrentSubscriptionOperations)
                {
                    return RuntimeClientSubscriptionWriteResult::NoWork;
                }

                RuntimeClientLogicalSubscriptionId unsubscribeId{};
                for (const auto& [logicalId, descriptor] : SubscriptionDescriptors)
                {
                    const auto view = Subscriptions.Find(logicalId);
                    if (!view)
                    {
                        continue;
                    }
                    if (view->Revision != descriptor.Revision)
                    {
                        inconsistent = true;
                        break;
                    }
                    if (_unsubscribeReady(*view, epoch->Epoch) &&
                        (unsubscribeId == 0 || logicalId < unsubscribeId))
                    {
                        unsubscribeId = logicalId;
                        SubscriptionWork work;
                        work.Kind = SubscriptionWorkKind::Unsubscribe;
                        work.Target = { logicalId, descriptor.Revision };
                        work.Unsubscribe.SubscriptionId = view->WireSubscriptionId;
                        work.Unsubscribe.Generation = view->Generation;
                        selected = std::move(work);
                    }
                }

                if (!inconsistent && !selected)
                {
                    if (const auto target = Subscriptions.NextNeedingSnapshot())
                    {
                        const auto descriptor = SubscriptionDescriptors.find(target->LogicalId);
                        if (descriptor == SubscriptionDescriptors.end() ||
                            descriptor->second.Revision != target->Revision)
                        {
                            inconsistent = true;
                        }
                        else if (descriptor->second.SnapshotAttempts <
                                 Configuration.MaximumSubscriptionAttempts)
                        {
                            SubscriptionWork work;
                            work.Kind = SubscriptionWorkKind::Snapshot;
                            work.Target = *target;
                            work.Attempt = descriptor->second.SnapshotAttempts + 1;
                            work.ParamsJson = descriptor->second.ParamsJson;
                            selected = std::move(work);
                        }
                    }
                }
            }

            if (inconsistent)
            {
                return retireProtocol(ERROR_INVALID_STATE, "Subscription writer state is inconsistent");
            }
            if (!selected)
            {
                return RuntimeClientSubscriptionWriteResult::NoWork;
            }

            std::optional<std::string> requestId;
            try
            {
                requestId = Dependencies.RequestIds->Next();
            }
            catch (...)
            {
                requestId.reset();
            }
            if (!requestId || !IsCanonicalProtocolUuid(*requestId) ||
                IsReservedHeartbeatRequestId(*requestId))
            {
                return retireProtocol(
                    ERROR_INVALID_DATA,
                    "Subscription request ID source returned an invalid identifier");
            }

            ControlMethod method{ ControlMethod::SnapshotAndSubscribe };
            std::string payloadJson;
            if (selected->Kind == SubscriptionWorkKind::Snapshot)
            {
                payloadJson = std::move(selected->ParamsJson);
            }
            else
            {
                method = ControlMethod::Unsubscribe;
                const auto unsubscribe = EncodeUnsubscribeRequest(selected->Unsubscribe);
                if (!unsubscribe.Succeeded)
                {
                    return retireProtocol(
                        ERROR_INVALID_DATA,
                        "Subscription writer could not encode Unsubscribe parameters");
                }
                payloadJson = std::move(unsubscribe.Payload);
            }

            ControlRequest request;
            request.RequestId = *requestId;
            request.Method = method;
            request.PayloadJson = payloadJson;
            const auto encoded = EncodeControlRequest(request);
            if (!encoded.Succeeded || encoded.Payload.size() > Configuration.MaximumRequestBytes)
            {
                return retireProtocol(
                    ERROR_INVALID_DATA,
                    "Subscription request exceeded its validated client payload limit");
            }
            if (encoded.Payload.size() > epoch->Hello.MaximumControlFrameBytes)
            {
                return RejectOversizedSubscriptionRequest(epoch, selected->Target);
            }

            RuntimeClientLogicalOperation staged;
            staged.Kind = selected->Kind == SubscriptionWorkKind::Snapshot ?
                              RuntimeClientLogicalOperationKind::SubscriptionSnapshot :
                              RuntimeClientLogicalOperationKind::SubscriptionUnsubscribe;
            staged.Subscription = selected->Target;
            staged.Method = method;
            staged.PayloadJson = std::move(payloadJson);
            staged.WireBytes = encoded.Payload.size();
            staged.CurrentAttempt = selected->Attempt;
            staged.MaximumAttempts = selected->Kind == SubscriptionWorkKind::Snapshot ?
                                         Configuration.MaximumSubscriptionAttempts :
                                         1;
            staged.CurrentRequestId = *requestId;

            RuntimeClientOperationId operationId{};
            bool bindingStarted{};
            bool requestAdded{};
            bool operationAdded{};
            bool committed{};
            bool benignRace{};
            bool rollbackFailed{};
            {
                std::scoped_lock lock{ Mutex };
                const auto rollback = [&]() noexcept {
                    if (bindingStarted)
                    {
                        const auto failed = Subscriptions.FailRequest(epoch->Epoch, *requestId);
                        rollbackFailed = failed != RuntimeClientSubscriptionStatus::Succeeded &&
                                         failed != RuntimeClientSubscriptionStatus::Finalized;
                        bindingStarted = false;
                    }
                    if (requestAdded)
                    {
                        try
                        {
                            (void)Requests.CancelQueued(operationId);
                        }
                        catch (...)
                        {
                            rollbackFailed = true;
                        }
                        requestAdded = false;
                    }
                    if (operationAdded)
                    {
                        EraseSubscriptionOperationLocked(operationId);
                        operationAdded = false;
                    }
                };

                try
                {
                    if (ClientState == TaskRuntimeClientState::Stopping ||
                        !Active || Active.get() != epoch.get() ||
                        !Epochs.IsActive(epoch->Epoch))
                    {
                        return RuntimeClientSubscriptionWriteResult::Stop;
                    }
                    if (HasHeartbeatWriterWorkLocked(epoch->Epoch))
                    {
                        return RuntimeClientSubscriptionWriteResult::NoWork;
                    }
                    if (SubscriptionOperationCount >=
                        Configuration.MaximumConcurrentSubscriptionOperations)
                    {
                        return RuntimeClientSubscriptionWriteResult::NoWork;
                    }

                    const auto descriptor = SubscriptionDescriptors.find(selected->Target.LogicalId);
                    const auto view = Subscriptions.Find(selected->Target.LogicalId);
                    if (descriptor == SubscriptionDescriptors.end() || !view ||
                        descriptor->second.Revision != selected->Target.Revision ||
                        view->Revision != selected->Target.Revision)
                    {
                        benignRace = true;
                    }
                    else if (selected->Kind == SubscriptionWorkKind::Snapshot &&
                             (view->Phase != RuntimeClientSubscriptionPhase::Desired &&
                              view->Phase != RuntimeClientSubscriptionPhase::NeedsSnapshot))
                    {
                        benignRace = true;
                    }
                    else if (selected->Kind == SubscriptionWorkKind::Unsubscribe &&
                             !_unsubscribeReady(*view, epoch->Epoch))
                    {
                        benignRace = true;
                    }
                    else if (selected->Kind == SubscriptionWorkKind::Snapshot &&
                             (descriptor->second.SnapshotAttempts + 1 != selected->Attempt ||
                              selected->Attempt > Configuration.MaximumSubscriptionAttempts))
                    {
                        benignRace = true;
                    }

                    if (!benignRace &&
                        (NextOperationId == 0 ||
                         IssuedRequestIds.size() >= Configuration.MaximumRememberedRequestIds ||
                         QueuedBytes >
                             Configuration.MaximumQueuedBytes - Configuration.MaximumRequestBytes ||
                         encoded.Payload.size() >
                             Configuration.MaximumQueuedBytes - Configuration.MaximumRequestBytes -
                                 QueuedBytes))
                    {
                        rollbackFailed = true;
                    }
                    if (!benignRace && !rollbackFailed &&
                        !IssuedRequestIds.emplace(*requestId).second)
                    {
                        rollbackFailed = true;
                    }
                    if (!benignRace && !rollbackFailed)
                    {
                        operationId = NextOperationId;
                        NextOperationId = NextOperationId ==
                                                  (std::numeric_limits<RuntimeClientOperationId>::max)() ?
                                              0 :
                                              NextOperationId + 1;
                        staged.OperationId = operationId;

                        const auto added = Requests.Add(RuntimeClientRequestAttemptInfo{
                            operationId,
                            selected->Attempt,
                            *requestId,
                            method,
                            0,
                            ClientRequestTransmission::NotSent,
                        });
                        requestAdded = added == RuntimeClientRequestAddStatus::Added;
                        rollbackFailed = !requestAdded;
                    }
                    if (!benignRace && !rollbackFailed)
                    {
                        const auto inserted = Operations.emplace(operationId, std::move(staged));
                        operationAdded = inserted.second;
                        rollbackFailed = !operationAdded;
                        if (operationAdded)
                        {
                            QueuedBytes += encoded.Payload.size();
                            ++SubscriptionOperationCount;
                        }
                    }
                    if (!benignRace && !rollbackFailed)
                    {
                        const auto bound = selected->Kind == SubscriptionWorkKind::Snapshot ?
                                               Subscriptions.BeginSnapshot(
                                                   selected->Target.LogicalId,
                                                   selected->Target.Revision,
                                                   epoch->Epoch,
                                                   *requestId) :
                                               Subscriptions.BeginUnsubscribe(
                                                   selected->Target.LogicalId,
                                                   selected->Target.Revision,
                                                   epoch->Epoch,
                                                   *requestId);
                        bindingStarted = bound == RuntimeClientSubscriptionStatus::Succeeded;
                        benignRace = bound == RuntimeClientSubscriptionStatus::NotFound ||
                                     bound == RuntimeClientSubscriptionStatus::Stale ||
                                     bound == RuntimeClientSubscriptionStatus::WrongPhase;
                        rollbackFailed = !bindingStarted && !benignRace;
                    }
                    if (!benignRace && !rollbackFailed)
                    {
                        if (!Requests.BeginWrite(*requestId, epoch->Epoch))
                        {
                            rollbackFailed = true;
                        }
                        else
                        {
                            if (selected->Kind == SubscriptionWorkKind::Snapshot)
                            {
                                descriptor->second.SnapshotAttempts = selected->Attempt;
                            }
                            committed = true;
                        }
                    }

                    if (!committed)
                    {
                        rollback();
                    }
                }
                catch (...)
                {
                    rollbackFailed = true;
                    rollback();
                }
            }

            if (benignRace && !rollbackFailed)
            {
                return RuntimeClientSubscriptionWriteResult::NoWork;
            }
            if (!committed || rollbackFailed)
            {
                return retireProtocol(
                    ERROR_INVALID_STATE,
                    "Subscription writer could not commit internal request state");
            }

            auto sent = epoch->Connection->Send(
                encoded.Payload,
                Configuration.SendTimeoutMilliseconds,
                epoch->Cancellation.Get());
            if (!sent.Succeeded())
            {
                RequestEpochRetire(
                    epoch,
                    RuntimeClientRetireCause::TransportFailure,
                    sent.Status,
                    sent.SystemError,
                    sent.Error);
                return RuntimeClientSubscriptionWriteResult::RetireEpoch;
            }
            {
                std::scoped_lock lock{ Mutex };
                (void)Requests.MarkWriteCompleted(*requestId, epoch->Epoch);
            }
            return RuntimeClientSubscriptionWriteResult::Sent;
        }
        catch (...)
        {
            return retireProtocol(
                ERROR_UNHANDLED_EXCEPTION,
                "Unexpected subscription writer failure");
        }
    }

    void RuntimeClientSharedState::EraseSubscriptionOperationLocked(
        const RuntimeClientOperationId operationId) noexcept
    {
        const auto operation = Operations.find(operationId);
        if (operation == Operations.end() || !_internalOperation(operation->second.Kind))
        {
            return;
        }
        QueuedBytes = operation->second.WireBytes <= QueuedBytes ?
                          QueuedBytes - operation->second.WireBytes :
                          0;
        if (SubscriptionOperationCount != 0)
        {
            --SubscriptionOperationCount;
        }
        Operations.erase(operation);
        WriterWake.notify_all();
    }
}
