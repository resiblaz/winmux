// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlServerInternal.h"

#include "RuntimeControlChannel.h"
#include "RuntimeControlPublisher.h"
#include "RuntimeControlThreadContext.h"

#include <stdexcept>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        bool _isSlowClientReason(const RuntimeControlSessionCloseReason reason) noexcept
        {
            return reason == RuntimeControlSessionCloseReason::OutboundQueueLimit ||
                   reason == RuntimeControlSessionCloseReason::WriteTimeout;
        }
    }

    RuntimeControlSessionConfiguration RuntimeControlServer::Impl::SessionConfiguration() const
    {
        RuntimeControlSessionConfiguration session;
        session.Handshake = Configuration.Handshake;
        session.HandshakeTimeoutMilliseconds = Configuration.HandshakeTimeoutMilliseconds;
        session.ReadTimeoutMilliseconds = Configuration.ReadTimeoutMilliseconds;
        session.WriteTimeoutMilliseconds = Configuration.WriteTimeoutMilliseconds;
        session.MaximumInflightRequests = Configuration.MaximumInflightRequests;
        session.MaximumQueuedMessages = Configuration.MaximumQueuedMessages;
        session.MaximumQueuedBytes = Configuration.MaximumQueuedBytes;
        session.MaximumSubscriptions = Configuration.MaximumSubscriptionsPerClient;
        session.CompletionEvent = SessionCompletedEvent;
        return session;
    }

    void RuntimeControlServer::Impl::RecordCompletion(
        const RuntimeControlSessionCloseReason reason) noexcept
    {
        CompletedConnections.fetch_add(1, std::memory_order_relaxed);
        if (reason == RuntimeControlSessionCloseReason::HandshakeRejected)
        {
            HandshakeRejectedConnections.fetch_add(1, std::memory_order_relaxed);
        }
        else if (_isSlowClientReason(reason))
        {
            SlowClientDisconnects.fetch_add(1, std::memory_order_relaxed);
        }
        else if (reason == RuntimeControlSessionCloseReason::MalformedRequest)
        {
            MalformedConnections.fetch_add(1, std::memory_order_relaxed);
        }
        else if (reason == RuntimeControlSessionCloseReason::TransportError)
        {
            TransportFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void RuntimeControlServer::Impl::ReapFinishedSessions()
    {
        std::vector<std::unique_ptr<RuntimeControlSession>> finished;
        {
            std::scoped_lock lock{ SessionsMutex };
            auto iterator = Sessions.begin();
            while (iterator != Sessions.end())
            {
                if ((*iterator)->Finished())
                {
                    finished.emplace_back(std::move(*iterator));
                    iterator = Sessions.erase(iterator);
                }
                else
                {
                    ++iterator;
                }
            }
        }
        for (auto& session : finished)
        {
            session->Join();
            RecordCompletion(session->CloseReason());
        }
    }

    size_t RuntimeControlServer::Impl::ActiveSessionCount() const noexcept
    {
        std::scoped_lock lock{ SessionsMutex };
        return Sessions.size();
    }

    void RuntimeControlServer::Impl::CancelListener() noexcept
    {
        std::scoped_lock lock{ ListenerMutex };
        if (ActiveListener != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(ActiveListener, nullptr);
        }
    }

    void RuntimeControlServer::Impl::StopAndJoinSessions() noexcept
    {
        std::vector<std::unique_ptr<RuntimeControlSession>> sessions;
        {
            std::scoped_lock lock{ SessionsMutex };
            sessions.swap(Sessions);
        }
        for (auto& session : sessions)
        {
            session->RequestStop(RuntimeControlSessionCloseReason::ServerStopping);
        }
        for (auto& session : sessions)
        {
            session->Join();
            RecordCompletion(session->CloseReason());
        }
    }

    void RuntimeControlServer::Impl::RunAcceptor() noexcept
    {
        const RuntimeControlThreadScope workerScope;
        auto terminalStatus = RuntimeControlServerTerminalStatus::Stopped;
        try
        {
            auto listener = std::move(InitialListener);
            while (!StopRequested.load(std::memory_order_acquire))
            {
                if (!listener)
                {
                    auto created = CreateRuntimeControlPipe(Identity, Security.Attributes(), false);
                    if (!created.Created())
                    {
                        TransportFailures.fetch_add(1, std::memory_order_relaxed);
                        terminalStatus = RuntimeControlServerTerminalStatus::TransportFailure;
                        break;
                    }
                    listener = std::move(created.Pipe);
                }

                {
                    std::scoped_lock lock{ ListenerMutex };
                    ActiveListener = listener.Handle();
                }
                auto accepted = AcceptRuntimeControlChannel(
                    listener,
                    Configuration.AcceptTimeoutMilliseconds,
                    StopEvent);
                {
                    std::scoped_lock lock{ ListenerMutex };
                    ActiveListener = INVALID_HANDLE_VALUE;
                }
                if (StopRequested.load(std::memory_order_acquire))
                {
                    break;
                }
                if (!accepted.Succeeded())
                {
                    listener = {};
                    ReapFinishedSessions();
                    if (accepted.Status == RuntimeTransportStatus::Timeout)
                    {
                        continue;
                    }
                    TransportFailures.fetch_add(1, std::memory_order_relaxed);
                    terminalStatus = RuntimeControlServerTerminalStatus::TransportFailure;
                    break;
                }

                AcceptedConnections.fetch_add(1, std::memory_order_relaxed);
                ReapFinishedSessions();
                if (ActiveSessionCount() >= Configuration.MaximumClients)
                {
                    CapacityRejectedConnections.fetch_add(1, std::memory_order_relaxed);
                    accepted.Channel.Close();
                    continue;
                }

                auto session = std::make_unique<RuntimeControlSession>(
                    std::move(accepted.Channel),
                    Router,
                    SessionConfiguration(),
                    SubscriptionOwnershipMutex);
                if (!session->Start())
                {
                    TransportFailures.fetch_add(1, std::memory_order_relaxed);
                    session->RequestStop(RuntimeControlSessionCloseReason::TransportError);
                    session->Join();
                    continue;
                }
                {
                    std::scoped_lock lock{ SessionsMutex };
                    Sessions.emplace_back(std::move(session));
                }
                // The workers may have completed before Start returned. Wake
                // the Reaper after publication so that completion cannot be
                // lost and the local object cannot be reclaimed mid-Start.
                if (SessionCompletedEvent)
                {
                    SetEvent(SessionCompletedEvent);
                }
            }
        }
        catch (...)
        {
            terminalStatus = RuntimeControlServerTerminalStatus::InternalFailure;
        }

        if (InternalFailureRequested.load(std::memory_order_acquire))
        {
            terminalStatus = RuntimeControlServerTerminalStatus::InternalFailure;
        }

        {
            std::scoped_lock lock{ ListenerMutex };
            ActiveListener = INVALID_HANDLE_VALUE;
        }
        StopRequested.store(true, std::memory_order_release);
        if (StopEvent)
        {
            SetEvent(StopEvent);
        }
        if (SessionCompletedEvent)
        {
            SetEvent(SessionCompletedEvent);
        }
        StopAndJoinSessions();
        TerminalState.store(terminalStatus, std::memory_order_release);
        IsRunning.store(false, std::memory_order_release);
        if (StoppedEvent)
        {
            SetEvent(StoppedEvent);
        }
    }

    void RuntimeControlServer::Impl::RunReaper() noexcept
    {
        const RuntimeControlThreadScope workerScope;
        try
        {
            while (!StopRequested.load(std::memory_order_acquire))
            {
                ResetEvent(SessionCompletedEvent);
                ReapFinishedSessions();
                if (StopRequested.load(std::memory_order_acquire))
                {
                    break;
                }
                const auto waited = WaitForSingleObject(SessionCompletedEvent, INFINITE);
                if (waited != WAIT_OBJECT_0)
                {
                    throw std::runtime_error{ "Runtime control reaper wait failed" };
                }
            }
        }
        catch (...)
        {
            InternalFailureRequested.store(true, std::memory_order_release);
            RequestStop();
        }
    }

    bool RuntimeControlServer::Impl::Start() noexcept
    {
        if (!StopEvent || !SessionCompletedEvent || !StoppedEvent)
        {
            return false;
        }
        try
        {
            Sessions.reserve(Configuration.MaximumClients);
            TerminalState.store(RuntimeControlServerTerminalStatus::Running, std::memory_order_release);
            IsRunning.store(true, std::memory_order_release);
            Reaper = std::thread{ [this] { RunReaper(); } };
            Acceptor = std::thread{ [this] { RunAcceptor(); } };
            return true;
        }
        catch (...)
        {
            RequestStop();
            if (Reaper.joinable())
            {
                Reaper.join();
            }
            IsRunning.store(false, std::memory_order_release);
            TerminalState.store(RuntimeControlServerTerminalStatus::NotStarted, std::memory_order_release);
            return false;
        }
    }

    void RuntimeControlServer::Impl::RequestStop() noexcept
    {
        StopRequested.store(true, std::memory_order_release);
        if (StopEvent)
        {
            SetEvent(StopEvent);
        }
        if (SessionCompletedEvent)
        {
            SetEvent(SessionCompletedEvent);
        }
    }

    void RuntimeControlServer::Impl::Stop() noexcept
    {
        RequestStop();
        if (IsRuntimeControlWorkerThread())
        {
            return;
        }

        std::scoped_lock stopLock{ StopMutex };
        RequestStop();
        CancelListener();
        if (Acceptor.joinable())
        {
            Acceptor.join();
        }
        if (Reaper.joinable())
        {
            Reaper.join();
        }
        StopAndJoinSessions();
        IsRunning.store(false, std::memory_order_release);
    }

    RuntimeControlServerStatistics RuntimeControlServer::Impl::Statistics() const noexcept
    {
        RuntimeControlServerStatistics statistics;
        statistics.AcceptedConnections = AcceptedConnections.load(std::memory_order_relaxed);
        statistics.ActiveConnections = ActiveSessionCount();
        statistics.CompletedConnections = CompletedConnections.load(std::memory_order_relaxed);
        statistics.HandshakeRejectedConnections = HandshakeRejectedConnections.load(std::memory_order_relaxed);
        statistics.SlowClientDisconnects = SlowClientDisconnects.load(std::memory_order_relaxed);
        statistics.MalformedConnections = MalformedConnections.load(std::memory_order_relaxed);
        statistics.CapacityRejectedConnections = CapacityRejectedConnections.load(std::memory_order_relaxed);
        statistics.TransportFailures = TransportFailures.load(std::memory_order_relaxed);
        return statistics;
    }

    RuntimeControlPublishStatus RuntimeControlServer::Impl::PublishNotification(
        const ControlNotification& notification)
    {
        return PublishRuntimeControlNotification(
            notification,
            SessionsMutex,
            Sessions,
            *SubscriptionOwnershipMutex,
            StopRequested,
            IsRunning);
    }
}
