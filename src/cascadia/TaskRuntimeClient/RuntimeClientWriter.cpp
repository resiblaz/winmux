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
        bool _sameItem(
            const RuntimeClientWriterItem& left,
            const RuntimeClientWriterItem& right) noexcept
        {
            return left.OperationId == right.OperationId && left.Attempt == right.Attempt &&
                   left.RequestId == right.RequestId;
        }

        void _deliverOne(std::optional<RuntimeClientPendingCompletion> completion) noexcept
        {
            if (!completion || !completion->Promise)
            {
                return;
            }
            try
            {
                completion->Promise->set_value(std::move(completion->Result));
            }
            catch (...)
            {
            }
        }
    }

    bool RuntimeClientSharedState::HasHeartbeatWriterWorkLocked(
        const RuntimeClientEpoch epoch) const noexcept
    {
        return epoch != 0 && Active && Active->Epoch == epoch && Epochs.IsActive(epoch) &&
               Heartbeat.HasWriterWork();
    }

    RuntimeClientHeartbeatWriteResult RuntimeClientSharedState::RunHeartbeatWriter(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch) noexcept
    {
        if (!epoch)
        {
            return RuntimeClientHeartbeatWriteResult::Stop;
        }

        const auto retire = [&](const DWORD error, const std::string_view diagnostic) noexcept {
            RequestEpochRetire(
                epoch,
                RuntimeClientRetireCause::ProtocolError,
                RuntimeTransportStatus::SystemError,
                error,
                diagnostic);
            return RuntimeClientHeartbeatWriteResult::RetireEpoch;
        };

        try
        {
            uint64_t sequence{};
            bool sequenceExhausted{};
            {
                std::scoped_lock lock{ Mutex };
                if (ClientState == TaskRuntimeClientState::Stopping ||
                    !Active || Active.get() != epoch.get() ||
                    !Epochs.IsActive(epoch->Epoch))
                {
                    return RuntimeClientHeartbeatWriteResult::Stop;
                }
                if (!Heartbeat.HasWriterWork())
                {
                    return RuntimeClientHeartbeatWriteResult::NoWork;
                }
                if (LastHeartbeatSequence == (std::numeric_limits<uint64_t>::max)())
                {
                    sequenceExhausted = true;
                }
                else
                {
                    // Consume the sequence before any allocation. A failed ID
                    // generation therefore cannot cause reuse in a later epoch.
                    sequence = ++LastHeartbeatSequence;
                }
            }
            if (sequenceExhausted)
            {
                return retire(
                    ERROR_ARITHMETIC_OVERFLOW,
                    "Task Runtime heartbeat request sequence is exhausted");
            }

            const auto requestId = HeartbeatRequestNamespace ?
                                       MakeRuntimeClientHeartbeatRequestId(
                                           *HeartbeatRequestNamespace,
                                           sequence) :
                                       std::nullopt;
            if (!requestId)
            {
                return retire(
                    ERROR_INVALID_DATA,
                    "Task Runtime heartbeat request ID generation failed");
            }

            ControlRequest request;
            request.RequestId = *requestId;
            request.Method = ControlMethod::Ping;
            request.PayloadJson = "{}";
            const auto encoded = EncodeControlRequest(request);
            if (!encoded.Succeeded || encoded.Payload.size() > Configuration.MaximumRequestBytes ||
                encoded.Payload.size() > epoch->Hello.MaximumControlFrameBytes)
            {
                return retire(
                    ERROR_INVALID_DATA,
                    "Task Runtime heartbeat request exceeded the negotiated payload limit");
            }

            RuntimeClientOperationId operationId{};
            bool committed{};
            DWORD commitError{};
            std::string_view commitDiagnostic;
            {
                std::scoped_lock lock{ Mutex };
                if (ClientState == TaskRuntimeClientState::Stopping ||
                    !Active || Active.get() != epoch.get() ||
                    !Epochs.IsActive(epoch->Epoch))
                {
                    return RuntimeClientHeartbeatWriteResult::Stop;
                }
                if (!Heartbeat.HasWriterWork())
                {
                    return RuntimeClientHeartbeatWriteResult::NoWork;
                }
                if (NextOperationId == 0)
                {
                    commitError = ERROR_ARITHMETIC_OVERFLOW;
                    commitDiagnostic = "Task Runtime operation identifier space is exhausted";
                }
                else if (HeartbeatQueuedBytes != 0)
                {
                    commitError = ERROR_INVALID_STATE;
                    commitDiagnostic = "Task Runtime heartbeat byte reservation is already in use";
                }
                else
                {
                    operationId = NextOperationId;
                    NextOperationId = NextOperationId ==
                                              (std::numeric_limits<RuntimeClientOperationId>::max)() ?
                                          0 :
                                          NextOperationId + 1;

                    bool operationAdded{};
                    bool requestAdded{};
                    bool writeBegan{};
                    try
                    {
                        RuntimeClientLogicalOperation operation;
                        operation.OperationId = operationId;
                        operation.Kind = RuntimeClientLogicalOperationKind::HeartbeatPing;
                        operation.Method = ControlMethod::Ping;
                        operation.PayloadJson = "{}";
                        operation.WireBytes = encoded.Payload.size();
                        operation.CurrentAttempt = 1;
                        operation.MaximumAttempts = 1;
                        operation.CurrentRequestId = *requestId;

                        operationAdded = Operations.emplace(operationId, std::move(operation)).second;
                        if (operationAdded)
                        {
                            requestAdded = Requests.Add(RuntimeClientRequestAttemptInfo{
                                               operationId,
                                               1,
                                               *requestId,
                                               ControlMethod::Ping,
                                               0,
                                               ClientRequestTransmission::NotSent,
                                           }) == RuntimeClientRequestAddStatus::Added;
                        }
                        if (requestAdded)
                        {
                            writeBegan = Requests.BeginWrite(*requestId, epoch->Epoch);
                        }
                        if (writeBegan)
                        {
                            committed = Heartbeat.BeginPing(operationId);
                            if (committed)
                            {
                                HeartbeatQueuedBytes = encoded.Payload.size();
                            }
                        }
                    }
                    catch (...)
                    {
                    }

                    if (!committed)
                    {
                        try
                        {
                            if (writeBegan)
                            {
                                (void)Requests.SupersedeActive(operationId, epoch->Epoch);
                            }
                            else if (requestAdded)
                            {
                                (void)Requests.CancelQueued(operationId);
                            }
                        }
                        catch (...)
                        {
                        }
                        if (operationAdded)
                        {
                            Operations.erase(operationId);
                        }
                        commitError = ERROR_INVALID_STATE;
                        commitDiagnostic =
                            "Task Runtime heartbeat request could not enter the write state";
                    }
                }
            }
            if (!committed)
            {
                return retire(
                    commitError == ERROR_SUCCESS ? ERROR_INVALID_STATE : commitError,
                    commitDiagnostic.empty() ?
                        "Task Runtime heartbeat request could not be committed" :
                        commitDiagnostic);
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
                return RuntimeClientHeartbeatWriteResult::RetireEpoch;
            }

            bool inconsistent{};
            {
                std::scoped_lock lock{ Mutex };
                if (!Requests.MarkWriteCompleted(*requestId, epoch->Epoch))
                {
                    // A reader may acknowledge or supersede the Ping while
                    // Send is still returning. Its atomic cleanup makes a
                    // missing operation the expected benign result.
                    const auto operation = Operations.find(operationId);
                    inconsistent = operation != Operations.end() &&
                                   operation->second.Kind ==
                                       RuntimeClientLogicalOperationKind::HeartbeatPing &&
                                   operation->second.CurrentRequestId == *requestId;
                }
            }
            if (inconsistent)
            {
                return retire(
                    ERROR_INVALID_STATE,
                    "Task Runtime heartbeat write completion was inconsistent");
            }
            return RuntimeClientHeartbeatWriteResult::Sent;
        }
        catch (...)
        {
            return retire(
                ERROR_UNHANDLED_EXCEPTION,
                "Unexpected Task Runtime heartbeat writer failure");
        }
    }

    void RuntimeClientSharedState::EraseHeartbeatOperationLocked(
        const RuntimeClientOperationId operationId) noexcept
    {
        const auto operation = Operations.find(operationId);
        if (operation == Operations.end() ||
            operation->second.Kind != RuntimeClientLogicalOperationKind::HeartbeatPing)
        {
            return;
        }
        HeartbeatQueuedBytes = operation->second.WireBytes <= HeartbeatQueuedBytes ?
                                   HeartbeatQueuedBytes - operation->second.WireBytes :
                                   0;
        Operations.erase(operation);
        WriterWake.notify_all();
    }

    void RuntimeClientSharedState::RunWriter(
        std::shared_ptr<RuntimeClientEpochContext> epoch) noexcept
    {
        {
            std::scoped_lock lock{ Mutex };
            WriterThreadId = std::this_thread::get_id();
        }
        try
        {
            while (true)
            {
                const auto heartbeatWrite = RunHeartbeatWriter(epoch);
                if (heartbeatWrite == RuntimeClientHeartbeatWriteResult::Sent)
                {
                    continue;
                }
                if (heartbeatWrite == RuntimeClientHeartbeatWriteResult::RetireEpoch ||
                    heartbeatWrite == RuntimeClientHeartbeatWriteResult::Stop)
                {
                    break;
                }

                const auto subscriptionWrite = RunSubscriptionWriter(epoch);
                if (subscriptionWrite == RuntimeClientSubscriptionWriteResult::Sent)
                {
                    continue;
                }
                if (subscriptionWrite == RuntimeClientSubscriptionWriteResult::RetireEpoch ||
                    subscriptionWrite == RuntimeClientSubscriptionWriteResult::Stop)
                {
                    break;
                }

                RuntimeClientWriterItem item;
                ControlMethod method{ ControlMethod::Ping };
                std::string payload;
                {
                    std::unique_lock lock{ Mutex };
                    WriterWake.wait(lock, [&] {
                        return ClientState == TaskRuntimeClientState::Stopping ||
                               !Active || Active.get() != epoch.get() ||
                               !Epochs.IsActive(epoch->Epoch) || !WriterQueue.empty() ||
                               HasHeartbeatWriterWorkLocked(epoch->Epoch) ||
                               HasSubscriptionWriterWorkLocked(epoch->Epoch);
                    });
                    if (ClientState == TaskRuntimeClientState::Stopping ||
                        !Active || Active.get() != epoch.get() ||
                        !Epochs.IsActive(epoch->Epoch))
                    {
                        break;
                    }
                    if (HasHeartbeatWriterWorkLocked(epoch->Epoch) ||
                        HasSubscriptionWriterWorkLocked(epoch->Epoch))
                    {
                        continue;
                    }
                    if (WriterQueue.empty())
                    {
                        continue;
                    }

                    item = WriterQueue.front();
                    const auto operation = Operations.find(item.OperationId);
                    if (operation == Operations.end() ||
                        operation->second.CurrentAttempt != item.Attempt ||
                        operation->second.CurrentRequestId != item.RequestId ||
                        operation->second.Phase != RuntimeClientLogicalPhase::AttemptQueued)
                    {
                        WriterQueue.pop_front();
                        continue;
                    }
                    method = operation->second.Method;
                    payload = operation->second.PayloadJson;
                }

                ControlRequest request;
                request.RequestId = item.RequestId;
                request.Method = method;
                request.PayloadJson = std::move(payload);
                const auto encoded = EncodeControlRequest(request);
                if (!encoded.Succeeded ||
                    encoded.Payload.size() > epoch->Hello.MaximumControlFrameBytes)
                {
                    std::optional<RuntimeClientPendingCompletion> completion;
                    {
                        std::scoped_lock lock{ Mutex };
                        if (!WriterQueue.empty() && _sameItem(WriterQueue.front(), item))
                        {
                            WriterQueue.pop_front();
                        }
                        (void)Requests.CancelQueued(item.OperationId);
                        RuntimeClientRequestResult result;
                        result.Outcome = RuntimeClientRequestOutcome::InvalidRequest;
                        result.LastRequestId = item.RequestId;
                        result.Diagnostic = "Request exceeds the negotiated control payload limit";
                        completion = CompleteOperationLocked(item.OperationId, std::move(result));
                    }
                    _deliverOne(std::move(completion));
                    continue;
                }

                bool beganWrite{};
                bool staleEpoch{};
                bool heartbeatPreempted{};
                {
                    std::scoped_lock lock{ Mutex };
                    if (!Active || Active.get() != epoch.get() ||
                        !Epochs.IsActive(epoch->Epoch) ||
                        ClientState == TaskRuntimeClientState::Stopping)
                    {
                        staleEpoch = true;
                    }
                    else if (HasHeartbeatWriterWorkLocked(epoch->Epoch))
                    {
                        heartbeatPreempted = true;
                    }
                    else if (!WriterQueue.empty() && _sameItem(WriterQueue.front(), item))
                    {
                        WriterQueue.pop_front();
                        beganWrite = Requests.BeginWrite(item.RequestId, epoch->Epoch);
                    }
                }
                if (staleEpoch)
                {
                    break;
                }
                if (heartbeatPreempted)
                {
                    continue;
                }
                if (!beganWrite)
                {
                    RequestEpochRetire(
                        epoch,
                        RuntimeClientRetireCause::ProtocolError,
                        RuntimeTransportStatus::SystemError,
                        ERROR_INVALID_STATE,
                        "Writer could not bind the queued request to the active epoch");
                    break;
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
                    break;
                }
                {
                    std::scoped_lock lock{ Mutex };
                    (void)Requests.MarkWriteCompleted(item.RequestId, epoch->Epoch);
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
                "Unexpected client writer failure");
        }
        {
            std::scoped_lock lock{ Mutex };
            if (WriterThreadId == std::this_thread::get_id())
            {
                WriterThreadId = {};
            }
        }
    }
}
