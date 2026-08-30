// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

#include "ProtocolRpcJson.h"

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
                RuntimeClientWriterItem item;
                ControlMethod method{ ControlMethod::Ping };
                std::string payload;
                {
                    std::unique_lock lock{ Mutex };
                    WriterWake.wait(lock, [&] {
                        return ClientState == TaskRuntimeClientState::Stopping ||
                               !Active || Active.get() != epoch.get() ||
                               !Epochs.IsActive(epoch->Epoch) || !WriterQueue.empty();
                    });
                    if (ClientState == TaskRuntimeClientState::Stopping ||
                        !Active || Active.get() != epoch.get() ||
                        !Epochs.IsActive(epoch->Epoch))
                    {
                        break;
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
                {
                    std::scoped_lock lock{ Mutex };
                    if (!Active || Active.get() != epoch.get() ||
                        !Epochs.IsActive(epoch->Epoch) ||
                        ClientState == TaskRuntimeClientState::Stopping)
                    {
                        staleEpoch = true;
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
