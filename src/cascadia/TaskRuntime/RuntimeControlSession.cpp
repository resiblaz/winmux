// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlSession.h"

#include "ProtocolRpcJson.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        ControlResponse _errorResponse(
            const std::string_view requestId,
            const ProtocolRpcErrorCode code,
            std::string message)
        {
            ControlResponse response;
            response.RequestId = requestId;
            response.Error = ProtocolRpcError{ code, std::move(message), "{}" };
            return response;
        }

        RuntimeControlSessionCloseReason _readCloseReason(const RuntimeTransportStatus status) noexcept
        {
            switch (status)
            {
            case RuntimeTransportStatus::Timeout:
                return RuntimeControlSessionCloseReason::IdleTimeout;
            case RuntimeTransportStatus::Disconnected:
                return RuntimeControlSessionCloseReason::PeerDisconnected;
            case RuntimeTransportStatus::MalformedFrame:
                return RuntimeControlSessionCloseReason::MalformedRequest;
            default:
                return RuntimeControlSessionCloseReason::TransportError;
            }
        }

        RuntimeControlSessionCloseReason _writeCloseReason(const RuntimeTransportStatus status) noexcept
        {
            switch (status)
            {
            case RuntimeTransportStatus::Timeout:
                return RuntimeControlSessionCloseReason::WriteTimeout;
            case RuntimeTransportStatus::Disconnected:
                return RuntimeControlSessionCloseReason::PeerDisconnected;
            default:
                return RuntimeControlSessionCloseReason::TransportError;
            }
        }
    }

    struct RuntimeControlSession::Impl
    {
        RuntimeControlChannel Channel;
        std::shared_ptr<IRuntimeControlRequestRouter> Router;
        RuntimeControlSessionConfiguration Configuration;
        ControlSession ProtocolSession;
        RuntimeControlQueue Outbound;
        RuntimeControlClientContext Client;
        std::shared_ptr<std::atomic_bool> CancellationState{ std::make_shared<std::atomic_bool>(false) };
        mutable std::mutex ProtocolMutex;
        std::thread Reader;
        std::thread Writer;
        std::atomic_bool ReaderDone{};
        std::atomic_bool WriterDone{};
        std::atomic_bool Started{};
        std::atomic<RuntimeControlSessionCloseReason> Reason{ RuntimeControlSessionCloseReason::None };

        Impl(
            RuntimeControlChannel channel,
            std::shared_ptr<IRuntimeControlRequestRouter> router,
            RuntimeControlSessionConfiguration configuration) :
            Channel{ std::move(channel) },
            Router{ std::move(router) },
            Configuration{ std::move(configuration) },
            ProtocolSession{ Configuration.Handshake, Configuration.MaximumInflightRequests },
            Outbound{ Configuration.MaximumQueuedMessages, Configuration.MaximumQueuedBytes }
        {
        }

        void RequestStop(const RuntimeControlSessionCloseReason reason) noexcept
        {
            auto expected = RuntimeControlSessionCloseReason::None;
            Reason.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);
            CancellationState->store(true, std::memory_order_release);
            Outbound.Close();
            const auto handle = Channel.Handle();
            if (handle != INVALID_HANDLE_VALUE)
            {
                CancelIoEx(handle, nullptr);
            }
        }

        [[nodiscard]] bool EnqueueResponse(
            const ControlResponse& response,
            const bool completeRequest)
        {
            auto encoded = EncodeControlResponse(response);
            if (!encoded.Succeeded)
            {
                encoded = EncodeControlResponse(_errorResponse(
                    response.RequestId,
                    ProtocolRpcErrorCode::InternalError,
                    "Runtime router returned an invalid response"));
            }
            if (!encoded.Succeeded)
            {
                RequestStop(RuntimeControlSessionCloseReason::TransportError);
                return false;
            }

            const auto queued = Outbound.Enqueue(
                std::move(encoded.Payload),
                completeRequest ? std::optional{ response.RequestId } : std::nullopt);
            if (queued == RuntimeControlQueueStatus::Accepted)
            {
                return true;
            }
            RequestStop(queued == RuntimeControlQueueStatus::QueueLimitReached ?
                            RuntimeControlSessionCloseReason::OutboundQueueLimit :
                            RuntimeControlSessionCloseReason::TransportError);
            return false;
        }

        void RunReader() noexcept
        {
            try
            {
                const auto handshake = PerformRuntimeServerHandshake(
                    Channel,
                    ProtocolSession,
                    Configuration.HandshakeTimeoutMilliseconds);
                if (!handshake.Succeeded())
                {
                    RequestStop(handshake.Status == RuntimeTransportStatus::ProtocolRejected ?
                                    RuntimeControlSessionCloseReason::HandshakeRejected :
                                    _readCloseReason(handshake.Status));
                    ReaderDone.store(true, std::memory_order_release);
                    return;
                }

                Client.ClientInstanceId = handshake.Request.ClientInstanceId;
                Client.ClientKind = handshake.Request.ClientKind;
                Client.NegotiatedCapabilities = handshake.Decision.Response.Capabilities;
                const RuntimeControlCancellationToken cancellation{ CancellationState };

                while (!cancellation.IsCancellationRequested())
                {
                    const auto received = Channel.Receive(Configuration.ReadTimeoutMilliseconds);
                    if (!received.Succeeded())
                    {
                        RequestStop(_readCloseReason(received.Status));
                        break;
                    }
                    const auto decoded = DecodeControlRequest(received.Payload);
                    if (!decoded.Succeeded())
                    {
                        RequestStop(RuntimeControlSessionCloseReason::MalformedRequest);
                        break;
                    }

                    ControlSessionDecision decision;
                    {
                        std::scoped_lock lock{ ProtocolMutex };
                        decision = ProtocolSession.BeginRequest(decoded.Value);
                    }
                    if (!decision.Accepted)
                    {
                        if (!EnqueueResponse(
                                _errorResponse(decoded.Value.RequestId, decision.Error, std::move(decision.Message)),
                                false))
                        {
                            break;
                        }
                        continue;
                    }

                    RuntimeControlRouteResult routed;
                    try
                    {
                        routed = Router->Route(Client, decoded.Value, cancellation);
                    }
                    catch (const std::exception&)
                    {
                        routed.Error = ProtocolRpcError{
                            ProtocolRpcErrorCode::InternalError,
                            "Runtime router failed",
                            "{}"
                        };
                    }
                    catch (...)
                    {
                        routed.Error = ProtocolRpcError{
                            ProtocolRpcErrorCode::InternalError,
                            "Runtime router failed",
                            "{}"
                        };
                    }
                    if (cancellation.IsCancellationRequested())
                    {
                        break;
                    }

                    ControlResponse response;
                    response.RequestId = decoded.Value.RequestId;
                    response.Succeeded = routed.Succeeded;
                    response.ResultJson = std::move(routed.ResultJson);
                    response.Error = std::move(routed.Error);
                    if (!EnqueueResponse(response, true))
                    {
                        break;
                    }
                }
            }
            catch (...)
            {
                RequestStop(RuntimeControlSessionCloseReason::TransportError);
            }
            ReaderDone.store(true, std::memory_order_release);
        }

        void RunWriter() noexcept
        {
            try
            {
                while (const auto payload = Outbound.WaitDequeue())
                {
                    const auto sent = Channel.Send(
                        payload->Payload,
                        Configuration.WriteTimeoutMilliseconds);
                    Outbound.Complete(*payload);
                    if (payload->CompletionRequestId)
                    {
                        std::scoped_lock lock{ ProtocolMutex };
                        (void)ProtocolSession.CompleteRequest(*payload->CompletionRequestId);
                    }
                    if (!sent.Succeeded())
                    {
                        RequestStop(_writeCloseReason(sent.Status));
                        break;
                    }
                }
            }
            catch (...)
            {
                RequestStop(RuntimeControlSessionCloseReason::TransportError);
            }
            WriterDone.store(true, std::memory_order_release);
        }

        void MarkClosed() noexcept
        {
            std::scoped_lock lock{ ProtocolMutex };
            ProtocolSession.MarkClosed();
        }
    };

    RuntimeControlSession::RuntimeControlSession(
        RuntimeControlChannel channel,
        std::shared_ptr<IRuntimeControlRequestRouter> router,
        RuntimeControlSessionConfiguration configuration) :
        _impl{ std::make_unique<Impl>(
            std::move(channel),
            std::move(router),
            std::move(configuration)) }
    {
    }

    RuntimeControlSession::~RuntimeControlSession()
    {
        RequestStop(RuntimeControlSessionCloseReason::ServerStopping);
        Join();
    }

    bool RuntimeControlSession::Start()
    {
        if (_impl->Started.exchange(true, std::memory_order_acq_rel) ||
            !_impl->Channel || !_impl->Router || !_impl->Outbound.Valid())
        {
            return false;
        }
        try
        {
            _impl->Writer = std::thread{ [this] { _impl->RunWriter(); } };
            _impl->Reader = std::thread{ [this] { _impl->RunReader(); } };
            return true;
        }
        catch (...)
        {
            _impl->RequestStop(RuntimeControlSessionCloseReason::TransportError);
            Join();
            return false;
        }
    }

    void RuntimeControlSession::RequestStop(const RuntimeControlSessionCloseReason reason) noexcept
    {
        _impl->RequestStop(reason);
    }

    void RuntimeControlSession::Join() noexcept
    {
        if (_impl->Reader.joinable())
        {
            _impl->Reader.join();
        }
        if (_impl->Writer.joinable())
        {
            _impl->Writer.join();
        }
        _impl->MarkClosed();
    }

    bool RuntimeControlSession::Finished() const noexcept
    {
        return _impl->Started.load(std::memory_order_acquire) &&
               _impl->ReaderDone.load(std::memory_order_acquire) &&
               _impl->WriterDone.load(std::memory_order_acquire);
    }

    RuntimeControlSessionCloseReason RuntimeControlSession::CloseReason() const noexcept
    {
        return _impl->Reason.load(std::memory_order_acquire);
    }
}
