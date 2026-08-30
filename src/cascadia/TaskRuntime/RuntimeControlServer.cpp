// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlServer.h"

#include "RuntimeControlChannel.h"
#include "RuntimeControlPublisher.h"
#include "RuntimeControlQueue.h"
#include "RuntimeControlSession.h"

#include "ProtocolCapabilities.h"
#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        bool _validTimeout(const uint32_t value) noexcept
        {
            return value != 0 && value <= MaximumRuntimeTransportTimeoutMilliseconds;
        }

        bool _validConfiguration(
            const RuntimeIdentity& identity,
            const RuntimeSecurityAttributes& security,
            const RuntimeNamedPipe& initialListener,
            const RuntimeControlServerConfiguration& configuration,
            const std::shared_ptr<IRuntimeControlRequestRouter>& router) noexcept
        {
            const auto& handshake = configuration.Handshake;
            return !identity.ProtocolIdentity.empty() &&
                   identity.ProtocolIdentity == handshake.RuntimeIdentity &&
                   initialListener && initialListener.Name() == identity.ControlPipeName &&
                   security && router &&
                   IsCanonicalProtocolUuid(handshake.RuntimeInstanceId) &&
                   handshake.Versions.Major == CurrentProtocolMajor &&
                   handshake.Versions.MinimumMinor <= handshake.Versions.PreferredMinor &&
                   (handshake.Capabilities & ~AllProtocolCapabilities) == 0 &&
                   handshake.MaximumControlFrameBytes >= MinimumControlFrameBytes &&
                   handshake.MaximumControlFrameBytes <= MaximumControlFrameBytes &&
                   handshake.MaximumTerminalFrameBytes != 0 &&
                   handshake.MaximumTerminalFrameBytes <= MaximumTerminalFrameBytes &&
                   handshake.HeartbeatMilliseconds >= 250 && handshake.HeartbeatMilliseconds <= 60000 &&
                   _validTimeout(configuration.AcceptTimeoutMilliseconds) &&
                   _validTimeout(configuration.HandshakeTimeoutMilliseconds) &&
                   _validTimeout(configuration.ReadTimeoutMilliseconds) &&
                   _validTimeout(configuration.WriteTimeoutMilliseconds) &&
                   configuration.MaximumClients != 0 &&
                   configuration.MaximumClients <= AbsoluteMaximumRuntimeControlClients &&
                   configuration.MaximumInflightRequests != 0 &&
                   configuration.MaximumInflightRequests <= AbsoluteMaximumInflightRequests &&
                   configuration.MaximumQueuedMessages != 0 &&
                   configuration.MaximumQueuedMessages <= AbsoluteMaximumQueuedControlMessages &&
                   configuration.MaximumQueuedBytes >=
                       static_cast<size_t>(handshake.MaximumControlFrameBytes) + sizeof(uint32_t) &&
                   configuration.MaximumQueuedBytes <= AbsoluteMaximumQueuedControlBytes &&
                   configuration.MaximumSubscriptionsPerClient != 0 &&
                   configuration.MaximumSubscriptionsPerClient <=
                       AbsoluteMaximumSubscriptionsPerControlClient;
        }

        bool _isSlowClientReason(const RuntimeControlSessionCloseReason reason) noexcept
        {
            return reason == RuntimeControlSessionCloseReason::OutboundQueueLimit ||
                   reason == RuntimeControlSessionCloseReason::WriteTimeout;
        }
    }

    struct RuntimeControlServer::Impl
    {
        RuntimeIdentity Identity;
        RuntimeSecurityAttributes Security;
        RuntimeNamedPipe InitialListener;
        RuntimeControlServerConfiguration Configuration;
        std::shared_ptr<IRuntimeControlRequestRouter> Router;
        std::atomic_bool StopRequested{};
        std::atomic_bool IsRunning{};
        HANDLE StopEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        std::thread Acceptor;
        mutable std::mutex StopMutex;
        mutable std::mutex ListenerMutex;
        HANDLE ActiveListener{ INVALID_HANDLE_VALUE };
        mutable std::mutex SessionsMutex;
        std::shared_ptr<std::mutex> SubscriptionOwnershipMutex{ std::make_shared<std::mutex>() };
        std::vector<std::unique_ptr<RuntimeControlSession>> Sessions;
        std::atomic_uint64_t AcceptedConnections{};
        std::atomic_uint64_t CompletedConnections{};
        std::atomic_uint64_t HandshakeRejectedConnections{};
        std::atomic_uint64_t SlowClientDisconnects{};
        std::atomic_uint64_t MalformedConnections{};
        std::atomic_uint64_t CapacityRejectedConnections{};
        std::atomic_uint64_t TransportFailures{};

        Impl(
            RuntimeIdentity identity,
            RuntimeSecurityAttributes security,
            RuntimeNamedPipe initialListener,
            RuntimeControlServerConfiguration configuration,
            std::shared_ptr<IRuntimeControlRequestRouter> router) :
            Identity{ std::move(identity) },
            Security{ std::move(security) },
            InitialListener{ std::move(initialListener) },
            Configuration{ std::move(configuration) },
            Router{ std::move(router) }
        {
        }

        ~Impl()
        {
            if (StopEvent)
            {
                CloseHandle(StopEvent);
            }
        }

        RuntimeControlSessionConfiguration SessionConfiguration() const
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
            return session;
        }

        void RecordCompletion(const RuntimeControlSessionCloseReason reason) noexcept
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

        void ReapFinishedSessions()
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

        size_t ActiveSessionCount() const noexcept
        {
            std::scoped_lock lock{ SessionsMutex };
            return Sessions.size();
        }

        void CancelListener() noexcept
        {
            std::scoped_lock lock{ ListenerMutex };
            if (ActiveListener != INVALID_HANDLE_VALUE)
            {
                CancelIoEx(ActiveListener, nullptr);
            }
        }

        void StopAndJoinSessions() noexcept
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

        void RunAcceptor() noexcept
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
                auto* const sessionPointer = session.get();
                {
                    std::scoped_lock lock{ SessionsMutex };
                    Sessions.emplace_back(std::move(session));
                }
                if (!sessionPointer->Start())
                {
                    TransportFailures.fetch_add(1, std::memory_order_relaxed);
                    std::unique_ptr<RuntimeControlSession> failed;
                    {
                        std::scoped_lock lock{ SessionsMutex };
                        const auto found = std::ranges::find_if(Sessions, [&](const auto& candidate) {
                            return candidate.get() == sessionPointer;
                        });
                        if (found != Sessions.end())
                        {
                            failed = std::move(*found);
                            Sessions.erase(found);
                        }
                    }
                    if (failed)
                    {
                        failed->RequestStop(RuntimeControlSessionCloseReason::TransportError);
                        failed->Join();
                    }
                    continue;
                }
            }

            {
                std::scoped_lock lock{ ListenerMutex };
                ActiveListener = INVALID_HANDLE_VALUE;
            }
            {
                std::scoped_lock lock{ SessionsMutex };
                StopRequested.store(true, std::memory_order_release);
            }
            StopAndJoinSessions();
            IsRunning.store(false, std::memory_order_release);
        }

        bool Start() noexcept
        {
            if (!StopEvent)
            {
                return false;
            }
            IsRunning.store(true, std::memory_order_release);
            try
            {
                Acceptor = std::thread{ [this] { RunAcceptor(); } };
                return true;
            }
            catch (...)
            {
                IsRunning.store(false, std::memory_order_release);
                return false;
            }
        }

        void Stop() noexcept
        {
            std::scoped_lock stopLock{ StopMutex };
            {
                std::scoped_lock sessionsLock{ SessionsMutex };
                StopRequested.store(true, std::memory_order_release);
            }
            if (StopEvent)
            {
                SetEvent(StopEvent);
            }
            CancelListener();
            if (Acceptor.joinable())
            {
                Acceptor.join();
            }
            StopAndJoinSessions();
            IsRunning.store(false, std::memory_order_release);
        }

        RuntimeControlServerStatistics Statistics() const noexcept
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

        RuntimeControlPublishStatus PublishNotification(const ControlNotification& notification)
        {
            return PublishRuntimeControlNotification(
                notification,
                SessionsMutex,
                Sessions,
                *SubscriptionOwnershipMutex,
                StopRequested,
                IsRunning);
        }
    };

    RuntimeControlServer::RuntimeControlServer() noexcept = default;
    RuntimeControlServer::~RuntimeControlServer()
    {
        Stop();
    }
    RuntimeControlServer::RuntimeControlServer(RuntimeControlServer&&) noexcept = default;
    RuntimeControlServer& RuntimeControlServer::operator=(RuntimeControlServer&& other) noexcept
    {
        if (this != &other)
        {
            Stop();
            _impl = std::move(other._impl);
        }
        return *this;
    }

    RuntimeControlServer::RuntimeControlServer(std::unique_ptr<Impl> impl) noexcept :
        _impl{ std::move(impl) }
    {
    }

    RuntimeControlServer::operator bool() const noexcept
    {
        return static_cast<bool>(_impl);
    }

    bool RuntimeControlServer::Running() const noexcept
    {
        return _impl && _impl->IsRunning.load(std::memory_order_acquire);
    }

    RuntimeControlServerStatistics RuntimeControlServer::Statistics() const noexcept
    {
        return _impl ? _impl->Statistics() : RuntimeControlServerStatistics{};
    }

    RuntimeControlPublishStatus RuntimeControlServer::PublishNotification(
        const ControlNotification& notification)
    {
        return _impl ?
                   _impl->PublishNotification(notification) :
                   RuntimeControlPublishStatus::ServerStopped;
    }

    void RuntimeControlServer::Stop() noexcept
    {
        if (_impl)
        {
            _impl->Stop();
        }
    }

    RuntimeControlServerResult StartRuntimeControlServer(
        RuntimeIdentity identity,
        RuntimeSecurityAttributes security,
        RuntimeNamedPipe initialListener,
        RuntimeControlServerConfiguration configuration,
        std::shared_ptr<IRuntimeControlRequestRouter> router)
    {
        RuntimeControlServerResult result;
        if (!_validConfiguration(identity, security, initialListener, configuration, router))
        {
            result.Error = "Runtime control server configuration is invalid";
            return result;
        }
        try
        {
            auto impl = std::make_unique<RuntimeControlServer::Impl>(
                std::move(identity),
                std::move(security),
                std::move(initialListener),
                std::move(configuration),
                std::move(router));
            if (!impl->Start())
            {
                result.Status = RuntimeControlServerStatus::ThreadStartFailure;
                result.Error = "Runtime control acceptor thread could not start";
                return result;
            }
            result.Server = RuntimeControlServer{ std::move(impl) };
            result.Status = RuntimeControlServerStatus::Started;
            return result;
        }
        catch (const std::exception& exception)
        {
            result.Error = exception.what();
            return result;
        }
    }
}
