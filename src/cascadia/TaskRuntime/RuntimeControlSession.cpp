// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlSessionState.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
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

    RuntimeControlSession::Impl::Impl(
        RuntimeControlChannel channel,
        std::shared_ptr<IRuntimeControlRequestRouter> router,
        RuntimeControlSessionConfiguration configuration,
        std::shared_ptr<std::mutex> subscriptionOwnershipMutex) :
        Channel{ std::move(channel) },
        Router{ std::move(router) },
        Configuration{ std::move(configuration) },
        ProtocolSession{ Configuration.Handshake, Configuration.MaximumInflightRequests },
        Outbound{ Configuration.MaximumQueuedMessages, Configuration.MaximumQueuedBytes },
        SubscriptionOwnershipMutex{ std::move(subscriptionOwnershipMutex) },
        MaximumPayloadBytes{ Configuration.Handshake.MaximumControlFrameBytes },
        Subscriptions{
            Configuration.MaximumSubscriptions,
            Configuration.MaximumQueuedMessages == 0 ?
                0 :
                Configuration.MaximumQueuedMessages - 1,
            Configuration.MaximumQueuedBytes >=
                    static_cast<size_t>(Configuration.Handshake.MaximumControlFrameBytes) + sizeof(uint32_t) ?
                Configuration.MaximumQueuedBytes -
                    (static_cast<size_t>(Configuration.Handshake.MaximumControlFrameBytes) + sizeof(uint32_t)) :
                0,
            Configuration.Handshake.MaximumControlFrameBytes }
    {
    }

    RuntimeControlSession::Impl::~Impl()
    {
        if (CancellationEvent)
        {
            CloseHandle(CancellationEvent);
        }
    }

    void RuntimeControlSession::Impl::RequestStop(
        const RuntimeControlSessionCloseReason reason) noexcept
    {
        auto expected = RuntimeControlSessionCloseReason::None;
        Reason.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);
        CancellationState->store(true, std::memory_order_release);
        if (CancellationEvent)
        {
            SetEvent(CancellationEvent);
        }
        Channel.CancelPendingIo();
        if (SubscriptionOwnershipMutex)
        {
            std::scoped_lock ownershipLock{ *SubscriptionOwnershipMutex };
            Subscriptions.ClearAndClose(Outbound);
        }
        else
        {
            Outbound.Close();
        }
        std::optional<RuntimeControlClientContext> disconnectedClient;
        {
            std::scoped_lock clientLock{ ClientMutex };
            if (!Client.ClientInstanceId.empty() &&
                !RouterDisconnectNotified.exchange(true, std::memory_order_acq_rel))
            {
                disconnectedClient = Client;
            }
        }
        if (disconnectedClient)
        {
            Router->ClientDisconnected(*disconnectedClient);
        }
    }

    void RuntimeControlSession::Impl::RunWriter() noexcept
    {
        try
        {
            while (const auto payload = Outbound.WaitDequeue())
            {
                const auto sent = Channel.Send(
                    payload->Payload,
                    Configuration.WriteTimeoutMilliseconds,
                    CancellationEvent);
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

    void RuntimeControlSession::Impl::MarkClosed() noexcept
    {
        std::scoped_lock lock{ ProtocolMutex };
        ProtocolSession.MarkClosed();
    }

    bool RuntimeControlSession::Impl::OwnsSubscription(
        const std::string& subscriptionId,
        const uint64_t generation) const
    {
        return !CancellationState->load(std::memory_order_acquire) &&
               Subscriptions.Owns(subscriptionId, generation);
    }

    RuntimeControlNotificationStatus RuntimeControlSession::Impl::PublishNotification(
        const ControlNotification& notification)
    {
        return CancellationState->load(std::memory_order_acquire) ?
                   RuntimeControlNotificationStatus::Closed :
                   Subscriptions.Publish(Outbound, notification);
    }

    RuntimeControlSession::RuntimeControlSession(
        RuntimeControlChannel channel,
        std::shared_ptr<IRuntimeControlRequestRouter> router,
        RuntimeControlSessionConfiguration configuration,
        std::shared_ptr<std::mutex> subscriptionOwnershipMutex) :
        _impl{ std::make_unique<Impl>(
            std::move(channel),
            std::move(router),
            std::move(configuration),
            std::move(subscriptionOwnershipMutex)) }
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
            !_impl->Channel || !_impl->Router || !_impl->CancellationEvent || !_impl->Outbound.Valid() ||
            !_impl->Subscriptions.Valid() || !_impl->SubscriptionOwnershipMutex)
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

    bool RuntimeControlSession::OwnsSubscription(
        const std::string& subscriptionId,
        const uint64_t generation) const
    {
        return _impl->OwnsSubscription(subscriptionId, generation);
    }

    RuntimeControlNotificationStatus RuntimeControlSession::PublishNotification(
        const ControlNotification& notification)
    {
        return _impl->PublishNotification(notification);
    }
}
