// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlRouter.h"
#include "RuntimeIdentity.h"
#include "RuntimePipe.h"
#include "RuntimeSecurity.h"

#include "ControlSession.h"
#include "ProtocolHandshake.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr size_t DefaultMaximumRuntimeControlClients{ 32 };
    inline constexpr size_t AbsoluteMaximumRuntimeControlClients{ 64 };

    struct RuntimeControlServerConfiguration
    {
        Microsoft::Terminal::TaskRuntimeProtocol::RuntimeHandshakeConfiguration Handshake;
        uint32_t AcceptTimeoutMilliseconds{ 60000 };
        uint32_t HandshakeTimeoutMilliseconds{ 5000 };
        uint32_t ReadTimeoutMilliseconds{ 60000 };
        uint32_t WriteTimeoutMilliseconds{ 5000 };
        size_t MaximumClients{ DefaultMaximumRuntimeControlClients };
        size_t MaximumInflightRequests{
            Microsoft::Terminal::TaskRuntimeProtocol::DefaultMaximumInflightRequests
        };
        size_t MaximumQueuedMessages{ 64 };
        size_t MaximumQueuedBytes{ 8 * 1024 * 1024 };
        size_t MaximumSubscriptionsPerClient{ 64 };
    };

    struct RuntimeControlServerStatistics
    {
        uint64_t AcceptedConnections{};
        size_t ActiveConnections{};
        uint64_t CompletedConnections{};
        uint64_t HandshakeRejectedConnections{};
        uint64_t SlowClientDisconnects{};
        uint64_t MalformedConnections{};
        uint64_t CapacityRejectedConnections{};
        uint64_t TransportFailures{};
    };

    enum class RuntimeControlServerStatus
    {
        Started,
        InvalidConfiguration,
        ThreadStartFailure,
    };

    enum class RuntimeControlServerTerminalStatus
    {
        NotStarted,
        Running,
        Stopped,
        TransportFailure,
        InternalFailure,
    };

    enum class RuntimeControlPublishStatus
    {
        Delivered,
        Duplicate,
        NoSubscriber,
        AmbiguousSubscription,
        SequenceGap,
        InvalidNotification,
        SlowClientDisconnected,
        ServerStopped,
    };

    struct RuntimeControlServerResult;

    class RuntimeControlServer final
    {
    public:
        RuntimeControlServer() noexcept;
        ~RuntimeControlServer();

        RuntimeControlServer(const RuntimeControlServer&) = delete;
        RuntimeControlServer& operator=(const RuntimeControlServer&) = delete;
        RuntimeControlServer(RuntimeControlServer&&) noexcept;
        RuntimeControlServer& operator=(RuntimeControlServer&&) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] bool Running() const noexcept;
        [[nodiscard]] RuntimeControlServerTerminalStatus TerminalStatus() const noexcept;
        // Borrowed manual-reset event, stable for this Server's lifetime.
        [[nodiscard]] HANDLE StoppedHandle() const noexcept;
        [[nodiscard]] RuntimeControlServerStatistics Statistics() const noexcept;
        [[nodiscard]] RuntimeControlPublishStatus PublishNotification(
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlNotification& notification);
        void Stop() noexcept;

    private:
        struct Impl;
        explicit RuntimeControlServer(std::unique_ptr<Impl> impl) noexcept;
        friend struct RuntimeControlServerResult;
        friend RuntimeControlServerResult StartRuntimeControlServer(
            RuntimeIdentity identity,
            RuntimeSecurityAttributes security,
            RuntimeNamedPipe initialListener,
            RuntimeControlServerConfiguration configuration,
            std::shared_ptr<IRuntimeControlRequestRouter> router);
        std::unique_ptr<Impl> _impl;
    };

    struct RuntimeControlServerResult
    {
        RuntimeControlServerStatus Status{ RuntimeControlServerStatus::InvalidConfiguration };
        RuntimeControlServer Server;
        std::string Error;

        [[nodiscard]] bool Started() const noexcept
        {
            return Status == RuntimeControlServerStatus::Started;
        }
    };

    [[nodiscard]] RuntimeControlServerResult StartRuntimeControlServer(
        RuntimeIdentity identity,
        RuntimeSecurityAttributes security,
        RuntimeNamedPipe initialListener,
        RuntimeControlServerConfiguration configuration,
        std::shared_ptr<IRuntimeControlRequestRouter> router);
}
