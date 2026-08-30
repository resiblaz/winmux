// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlServer.h"
#include "RuntimeControlSession.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeControlServer::Impl
    {
        RuntimeIdentity Identity;
        RuntimeSecurityAttributes Security;
        RuntimeNamedPipe InitialListener;
        RuntimeControlServerConfiguration Configuration;
        std::shared_ptr<IRuntimeControlRequestRouter> Router;
        std::atomic_bool StopRequested{};
        std::atomic_bool IsRunning{};
        std::atomic<RuntimeControlServerTerminalStatus> TerminalState{
            RuntimeControlServerTerminalStatus::NotStarted
        };
        std::atomic_bool InternalFailureRequested{};
        HANDLE StopEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        HANDLE SessionCompletedEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        HANDLE StoppedEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        std::thread Acceptor;
        std::thread Reaper;
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
            std::shared_ptr<IRuntimeControlRequestRouter> router);
        ~Impl();

        [[nodiscard]] RuntimeControlSessionConfiguration SessionConfiguration() const;
        void RecordCompletion(RuntimeControlSessionCloseReason reason) noexcept;
        void ReapFinishedSessions();
        [[nodiscard]] size_t ActiveSessionCount() const noexcept;
        void CancelListener() noexcept;
        void StopAndJoinSessions() noexcept;
        void RunAcceptor() noexcept;
        void RunReaper() noexcept;
        [[nodiscard]] bool Start() noexcept;

        // RequestStop is safe from acceptor/session/router callbacks: it only
        // publishes cancellation and never takes a lock or joins a worker.
        void RequestStop() noexcept;
        // Stop joins only from an external owner thread and is idempotent.
        void Stop() noexcept;

        [[nodiscard]] RuntimeControlServerStatistics Statistics() const noexcept;
        [[nodiscard]] RuntimeControlPublishStatus PublishNotification(
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlNotification& notification);
    };
}
