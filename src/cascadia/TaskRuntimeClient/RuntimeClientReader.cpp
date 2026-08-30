// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

#include "ProtocolRpcJson.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    bool RuntimeClientSharedState::RetireForHeartbeatObservation(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        const RuntimeClientHeartbeatStatus status,
        const std::string_view diagnostic) noexcept
    {
        if (status == RuntimeClientHeartbeatStatus::Succeeded ||
            status == RuntimeClientHeartbeatStatus::Stale)
        {
            return false;
        }

        bool timedOut{};
        {
            std::scoped_lock lock{ Mutex };
            const auto view = Heartbeat.ActiveView();
            timedOut = view && view->Phase == RuntimeClientHeartbeatPhase::TimedOut;
        }
        RequestEpochRetire(
            epoch,
            timedOut ? RuntimeClientRetireCause::HeartbeatTimeout :
                       RuntimeClientRetireCause::ProtocolError,
            timedOut ? RuntimeTransportStatus::Timeout :
                       RuntimeTransportStatus::SystemError,
            timedOut ? ERROR_TIMEOUT : ERROR_INVALID_STATE,
            diagnostic);
        return true;
    }

    RuntimeClientHeartbeatStatus RuntimeClientSharedState::ObserveInboundLocked(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        const RuntimeClientMonotonicMilliseconds now) noexcept
    {
        if (!epoch || !Active || Active.get() != epoch.get() ||
            !Epochs.IsActive(epoch->Epoch))
        {
            return RuntimeClientHeartbeatStatus::Stale;
        }

        const auto observed = Heartbeat.ObserveInbound(now);
        if (observed.Status != RuntimeClientHeartbeatStatus::Succeeded)
        {
            return observed.Status;
        }
        if (observed.SupersededOperation)
        {
            const auto operation = Operations.find(*observed.SupersededOperation);
            if (operation == Operations.end() ||
                operation->second.Kind != RuntimeClientLogicalOperationKind::HeartbeatPing ||
                Requests.SupersedeActive(*observed.SupersededOperation, epoch->Epoch) !=
                    RuntimeClientRequestSupersedeStatus::Superseded)
            {
                return RuntimeClientHeartbeatStatus::Failed;
            }
            EraseHeartbeatOperationLocked(*observed.SupersededOperation);
            if (Operations.contains(*observed.SupersededOperation))
            {
                return RuntimeClientHeartbeatStatus::Failed;
            }
        }

        ConsecutiveFailures = 0;
        epoch->InboundActivity.Signal();
        return RuntimeClientHeartbeatStatus::Succeeded;
    }

    bool RuntimeClientSharedState::HandleHeartbeatResponse(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        ControlResponse& response,
        const RuntimeClientMonotonicMilliseconds now) noexcept
    {
        if (!IsReservedHeartbeatRequestId(response.RequestId))
        {
            return false;
        }

        bool protocolFault{};
        RuntimeClientHeartbeatStatus heartbeatStatus{ RuntimeClientHeartbeatStatus::Stale };
        try
        {
            const auto identity = ParseRuntimeClientHeartbeatRequestId(response.RequestId);
            {
                std::scoped_lock lock{ Mutex };
                if (!Active || Active.get() != epoch.get() ||
                    !Epochs.IsActive(epoch->Epoch))
                {
                    return true;
                }
                if (!identity || !HeartbeatRequestNamespace ||
                    identity->Namespace != *HeartbeatRequestNamespace ||
                    identity->Sequence > LastHeartbeatSequence)
                {
                    protocolFault = true;
                }
                else
                {
                    const auto heartbeat = Heartbeat.ActiveView();
                    const auto operation = heartbeat && heartbeat->PingOperationId != 0 ?
                                               Operations.find(heartbeat->PingOperationId) :
                                               Operations.end();
                    const auto matchesActive =
                        operation != Operations.end() &&
                        operation->second.Kind == RuntimeClientLogicalOperationKind::HeartbeatPing &&
                        operation->second.CurrentRequestId == response.RequestId;
                    if (matchesActive)
                    {
                        const auto correlated = Requests.CompleteResponse(
                            epoch->Epoch,
                            Active->Epoch,
                            response.RequestId);
                        if (correlated.Status != RuntimeClientResponseMatch::Matched ||
                            !correlated.Attempt ||
                            correlated.Attempt->OperationId != operation->first ||
                            correlated.Attempt->Attempt != 1 ||
                            correlated.Attempt->Method != ControlMethod::Ping ||
                            correlated.Attempt->RequestId != response.RequestId)
                        {
                            protocolFault = true;
                        }
                        else
                        {
                            const auto operationId = operation->first;
                            heartbeatStatus = Heartbeat.AcknowledgePing(operationId, now);
                            EraseHeartbeatOperationLocked(operationId);
                            if (heartbeatStatus == RuntimeClientHeartbeatStatus::Succeeded)
                            {
                                ConsecutiveFailures = 0;
                                epoch->InboundActivity.Signal();
                            }
                            else if (heartbeatStatus == RuntimeClientHeartbeatStatus::Stale ||
                                     heartbeatStatus == RuntimeClientHeartbeatStatus::Inactive)
                            {
                                protocolFault = true;
                            }
                        }
                    }
                    else if (heartbeat && heartbeat->PingOperationId != 0 &&
                             (operation == Operations.end() ||
                              operation->second.Kind !=
                                  RuntimeClientLogicalOperationKind::HeartbeatPing))
                    {
                        protocolFault = true;
                    }
                    // Same-namespace sequences no newer than LastIssued that
                    // do not match the active Ping are old ACKs. Ignore them
                    // without refreshing liveness.
                }
            }
        }
        catch (...)
        {
            protocolFault = true;
        }

        if (protocolFault)
        {
            RequestEpochRetire(
                epoch,
                RuntimeClientRetireCause::ProtocolError,
                RuntimeTransportStatus::MalformedFrame,
                ERROR_INVALID_DATA,
                "Heartbeat response correlation violated the active connection epoch");
        }
        else
        {
            (void)RetireForHeartbeatObservation(
                epoch,
                heartbeatStatus,
                "Heartbeat acknowledgement arrived at or after the silence deadline");
        }
        return true;
    }

    void RuntimeClientSharedState::RunReader(
        std::shared_ptr<RuntimeClientEpochContext> epoch) noexcept
    {
        {
            std::scoped_lock lock{ Mutex };
            ReaderThreadId = std::this_thread::get_id();
        }
        try
        {
            while (WaitForSingleObject(epoch->Cancellation.Get(), 0) == WAIT_TIMEOUT)
            {
                auto received = epoch->Connection->Receive(
                    Configuration.ReceiveTimeoutMilliseconds,
                    epoch->Cancellation.Get());
                if (!received.Succeeded())
                {
                    if (received.Status == RuntimeTransportStatus::Timeout)
                    {
                        continue;
                    }
                    if (received.Status == RuntimeTransportStatus::Cancelled &&
                        WaitForSingleObject(epoch->Cancellation.Get(), 0) == WAIT_OBJECT_0)
                    {
                        break;
                    }
                    RequestEpochRetire(
                        epoch,
                        received.Status == RuntimeTransportStatus::MalformedFrame ?
                            RuntimeClientRetireCause::ProtocolError :
                            RuntimeClientRetireCause::TransportFailure,
                        received.Status,
                        received.SystemError,
                        received.Error);
                    break;
                }

                auto envelope = DecodeClientControlEnvelope(received.Payload);
                if (envelope.Status != ProtocolRpcJsonStatus::Succeeded &&
                    envelope.Status != ProtocolRpcJsonStatus::UnknownNotification)
                {
                    RequestEpochRetire(
                        epoch,
                        RuntimeClientRetireCause::ProtocolError,
                        RuntimeTransportStatus::MalformedFrame,
                        ERROR_INVALID_DATA,
                        envelope.Error);
                    break;
                }

                RuntimeClientMonotonicMilliseconds inboundNow{};
                try
                {
                    inboundNow = Dependencies.Scheduler->Now();
                }
                catch (...)
                {
                    RequestEpochRetire(
                        epoch,
                        RuntimeClientRetireCause::ProtocolError,
                        RuntimeTransportStatus::SystemError,
                        ERROR_UNHANDLED_EXCEPTION,
                        "Task Runtime heartbeat clock failed while reading inbound traffic");
                    break;
                }
                if (std::holds_alternative<ControlNotification>(envelope.Value))
                {
                    auto notification = std::get<ControlNotification>(std::move(envelope.Value));
                    if (!HandleSubscriptionNotification(epoch, std::move(notification), inboundNow))
                    {
                        RequestEpochRetire(
                            epoch,
                            RuntimeClientRetireCause::ProtocolError,
                            RuntimeTransportStatus::MalformedFrame,
                            ERROR_INVALID_DATA,
                            "Notification does not belong to the active subscription epoch");
                        break;
                    }
                    continue;
                }

                auto response = std::get<ControlResponse>(std::move(envelope.Value));
                if (HandleHeartbeatResponse(epoch, response, inboundNow))
                {
                    continue;
                }
                if (HandleSubscriptionResponse(epoch, response, inboundNow))
                {
                    continue;
                }
                std::optional<RuntimeClientPendingCompletion> completion;
                bool protocolFault{};
                RuntimeClientHeartbeatStatus heartbeatStatus{ RuntimeClientHeartbeatStatus::Stale };
                {
                    std::scoped_lock lock{ Mutex };
                    if (!Active || Active.get() != epoch.get() ||
                        !Epochs.IsActive(epoch->Epoch))
                    {
                        break;
                    }
                    const auto correlated = Requests.CompleteResponse(
                        epoch->Epoch,
                        Active->Epoch,
                        response.RequestId);
                    if (correlated.Status == RuntimeClientResponseMatch::Matched &&
                        correlated.Attempt)
                    {
                        const auto operation = Operations.find(correlated.Attempt->OperationId);
                        if (operation == Operations.end() ||
                            operation->second.CurrentAttempt != correlated.Attempt->Attempt ||
                            operation->second.CurrentRequestId != correlated.Attempt->RequestId)
                        {
                            protocolFault = true;
                        }
                        else if (operation->second.Kind != RuntimeClientLogicalOperationKind::UserRequest)
                        {
                            EraseSubscriptionOperationLocked(operation->first);
                            protocolFault = true;
                        }
                        else
                        {
                            heartbeatStatus = ObserveInboundLocked(epoch, inboundNow);
                            RuntimeClientRequestResult result;
                            result.Outcome = response.Succeeded ?
                                                 RuntimeClientRequestOutcome::Succeeded :
                                                 RuntimeClientRequestOutcome::RpcError;
                            result.Epoch = epoch->Epoch;
                            result.LastRequestId = response.RequestId;
                            result.Response = std::move(response);
                            completion = CompleteOperationLocked(
                                operation->first,
                                std::move(result));
                        }
                    }
                    else if (correlated.Status == RuntimeClientResponseMatch::DuplicateOrLate ||
                             correlated.Status == RuntimeClientResponseMatch::StaleEpoch)
                    {
                        continue;
                    }
                    else
                    {
                        protocolFault = true;
                    }
                }

                if (completion)
                {
                    try
                    {
                        completion->Promise->set_value(std::move(completion->Result));
                    }
                    catch (...)
                    {
                    }
                }
                if (protocolFault)
                {
                    RequestEpochRetire(
                        epoch,
                        RuntimeClientRetireCause::ProtocolError,
                        RuntimeTransportStatus::MalformedFrame,
                        ERROR_INVALID_DATA,
                        "Response correlation violated the active connection epoch");
                    break;
                }
                if (RetireForHeartbeatObservation(
                        epoch,
                        heartbeatStatus,
                        "Valid response arrived at or after the heartbeat silence deadline"))
                {
                    break;
                }
            }
        }
        catch (...)
        {
            RequestEpochRetire(
                epoch,
                RuntimeClientRetireCause::ProtocolError,
                RuntimeTransportStatus::SystemError,
                ERROR_UNHANDLED_EXCEPTION,
                "Unexpected client reader failure");
        }
        {
            std::scoped_lock lock{ Mutex };
            if (ReaderThreadId == std::this_thread::get_id())
            {
                ReaderThreadId = {};
            }
        }
    }
}
