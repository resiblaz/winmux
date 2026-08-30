// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlServerInternal.h"

#include "ProtocolCapabilities.h"
#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

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
    }

    RuntimeControlServer::Impl::Impl(
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

    RuntimeControlServer::Impl::~Impl()
    {
        if (StopEvent)
        {
            CloseHandle(StopEvent);
        }
        if (SessionCompletedEvent)
        {
            CloseHandle(SessionCompletedEvent);
        }
        if (StoppedEvent)
        {
            CloseHandle(StoppedEvent);
        }
    }

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

    RuntimeControlServerTerminalStatus RuntimeControlServer::TerminalStatus() const noexcept
    {
        return _impl ?
                   _impl->TerminalState.load(std::memory_order_acquire) :
                   RuntimeControlServerTerminalStatus::NotStarted;
    }

    HANDLE RuntimeControlServer::StoppedHandle() const noexcept
    {
        return _impl ? _impl->StoppedEvent : nullptr;
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
