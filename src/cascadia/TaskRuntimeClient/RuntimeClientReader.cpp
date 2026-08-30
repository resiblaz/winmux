// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

#include "ProtocolRpcJson.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

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
                if (envelope.Status != ProtocolRpcJsonStatus::Succeeded)
                {
                    RequestEpochRetire(
                        epoch,
                        RuntimeClientRetireCause::ProtocolError,
                        RuntimeTransportStatus::MalformedFrame,
                        ERROR_INVALID_DATA,
                        envelope.Error);
                    break;
                }
                if (!std::holds_alternative<ControlResponse>(envelope.Value))
                {
                    RequestEpochRetire(
                        epoch,
                        RuntimeClientRetireCause::ProtocolError,
                        RuntimeTransportStatus::MalformedFrame,
                        ERROR_INVALID_DATA,
                        "Unexpected notification before subscription support is enabled");
                    break;
                }

                auto response = std::get<ControlResponse>(std::move(envelope.Value));
                std::optional<RuntimeClientPendingCompletion> completion;
                bool protocolFault{};
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
                        else
                        {
                            // A correlated response proves that this epoch is
                            // healthy. A successful Hello alone does not: a peer
                            // that repeatedly accepts and immediately disconnects
                            // must continue climbing the reconnect backoff.
                            ConsecutiveFailures = 0;
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
