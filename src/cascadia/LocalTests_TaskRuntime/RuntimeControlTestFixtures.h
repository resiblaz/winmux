// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlChannel.h"
#include "RuntimeControlHandshake.h"
#include "RuntimeControlServer.h"
#include "TaskRuntimeTestFixtures.h"

#include "ProtocolRpcJson.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace TaskRuntimeLocalTests::ControlFixtures
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;
    using namespace TaskRuntimeLocalTests::TestFixtures;

    inline constexpr std::string_view ClientOneId{ "10000000-0000-4000-8000-000000000011" };
    inline constexpr std::string_view ClientTwoId{ "10000000-0000-4000-8000-000000000012" };

    class FakeControlRouter final : public IRuntimeControlRequestRouter
    {
    public:
        using Handler = std::function<RuntimeControlRouteResult(
            const RuntimeControlClientContext&,
            const ControlRequest&,
            const RuntimeControlCancellationToken&)>;
        using Planner = std::function<std::optional<RuntimeControlSubscriptionPlan>(
            const RuntimeControlClientContext&,
            const ControlRequest&,
            const RuntimeControlCancellationToken&)>;

        void SetHandler(Handler handler)
        {
            std::scoped_lock lock{ _mutex };
            _handler = std::move(handler);
        }

        void SetSubscriptionPlanner(Planner planner)
        {
            std::scoped_lock lock{ _mutex };
            _planner = std::move(planner);
        }

        std::optional<RuntimeControlSubscriptionPlan> PlanSubscription(
            const RuntimeControlClientContext& client,
            const ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation) override
        {
            Planner planner;
            {
                std::scoped_lock lock{ _mutex };
                planner = _planner;
            }
            return planner ? planner(client, request, cancellation) : std::nullopt;
        }

        void CancelSubscriptionPlan(
            const RuntimeControlClientContext&,
            const RuntimeControlSubscriptionPlan&) noexcept override
        {
            CancelledPlans.fetch_add(1, std::memory_order_relaxed);
        }

        void ClientDisconnected(const RuntimeControlClientContext&) noexcept override
        {
            DisconnectedClients.fetch_add(1, std::memory_order_relaxed);
        }

        RuntimeControlRouteResult Route(
            const RuntimeControlClientContext& client,
            const ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation) override
        {
            Calls.fetch_add(1, std::memory_order_relaxed);
            Handler handler;
            {
                std::scoped_lock lock{ _mutex };
                handler = _handler;
            }
            if (handler)
            {
                return handler(client, request, cancellation);
            }
            if (request.PayloadJson.find("\"throw\":true") != std::string::npos)
            {
                throw std::runtime_error{ "simulated router failure" };
            }

            RuntimeControlRouteResult result;
            if (cancellation.IsCancellationRequested())
            {
                result.Error = ProtocolRpcError{
                    ProtocolRpcErrorCode::RuntimeUnavailable,
                    "request cancelled",
                    "{}"
                };
                return result;
            }
            result.Succeeded = true;
            if (request.PayloadJson.find("\"large\":true") != std::string::npos)
            {
                result.ResultJson = "{\"padding\":\"" + std::string(128 * 1024, 'x') + "\"}";
            }
            else
            {
                result.ResultJson = "{\"clientInstanceId\":\"" + client.ClientInstanceId + "\"}";
            }
            return result;
        }

        std::atomic_size_t Calls{};
        std::atomic_size_t CancelledPlans{};
        std::atomic_size_t DisconnectedClients{};

    private:
        std::mutex _mutex;
        Handler _handler;
        Planner _planner;
    };

    inline RuntimeHandshakeConfiguration RuntimeConfiguration(const RuntimeIdentity& identity)
    {
        RuntimeHandshakeConfiguration runtime;
        runtime.RuntimeIdentity = identity.ProtocolIdentity;
        runtime.RuntimeInstanceId = "20000000-0000-4000-8000-000000000020";
        runtime.Capabilities = AllProtocolCapabilities;
        runtime.MaximumControlFrameBytes = 256 * 1024;
        runtime.MaximumTerminalFrameBytes = 64 * 1024;
        runtime.HeartbeatMilliseconds = 1000;
        return runtime;
    }

    inline HelloRequest Hello(
        const RuntimeIdentity& identity,
        const std::string_view clientId,
        const ProtocolCapabilitySet optionalCapabilities = AllProtocolCapabilities,
        const uint32_t maximumAcceptedControlFrameBytes = 256 * 1024)
    {
        HelloRequest request;
        request.ClientInstanceId = clientId;
        request.ClientKind = ProtocolClientKind::TestPeer;
        request.ExpectedRuntimeIdentity = identity.ProtocolIdentity;
        request.OptionalCapabilities = optionalCapabilities;
        request.MaximumAcceptedControlFrameBytes = maximumAcceptedControlFrameBytes;
        return request;
    }

    struct RunningControlServer
    {
        UniqueRuntimeRoot Root;
        RuntimeIdentity Identity;
        std::shared_ptr<FakeControlRouter> Router{ std::make_shared<FakeControlRouter>() };
        RuntimeControlServer Server;

        explicit RunningControlServer(
            RuntimeControlServerConfiguration configuration = {},
            const ProtocolCapabilitySet runtimeCapabilities = AllProtocolCapabilities)
        {
            Identity = RequireIdentity(Root.Path());
            auto security = RequireSecurity(Identity.UserSid);
            auto listener = CreateRuntimeControlPipe(Identity, security.Attributes(), true);
            VERIFY_IS_TRUE(listener.Created());
            configuration.Handshake = RuntimeConfiguration(Identity);
            configuration.Handshake.Capabilities = runtimeCapabilities;
            auto started = StartRuntimeControlServer(
                Identity,
                std::move(security),
                std::move(listener.Pipe),
                std::move(configuration),
                Router);
            VERIFY_IS_TRUE(started.Started());
            Server = std::move(started.Server);
            VERIFY_IS_TRUE(Server.Running());
        }
    };

    inline RuntimeControlChannel ConnectAndHello(
        RunningControlServer& server,
        const std::string_view clientId,
        const ProtocolCapabilitySet optionalCapabilities = AllProtocolCapabilities,
        const uint32_t maximumAcceptedControlFrameBytes = 256 * 1024)
    {
        auto connected = ConnectRuntimeControlChannel(server.Identity, 5000);
        VERIFY_IS_TRUE(connected.Succeeded());
        const auto handshake = PerformRuntimeClientHandshake(
            connected.Channel,
            Hello(
                server.Identity,
                clientId,
                optionalCapabilities,
                maximumAcceptedControlFrameBytes),
            5000);
        VERIFY_IS_TRUE(handshake.Succeeded());
        return std::move(connected.Channel);
    }

    inline void SendRequest(
        RuntimeControlChannel& channel,
        const std::string_view requestId,
        const ControlMethod method,
        std::string payload = "{}")
    {
        ControlRequest request;
        request.RequestId = requestId;
        request.Method = method;
        request.PayloadJson = std::move(payload);
        const auto encoded = EncodeControlRequest(request);
        VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_IS_TRUE(channel.Send(encoded.Payload, 5000).Succeeded());
    }

    inline ControlResponse ReceiveResponse(RuntimeControlChannel& channel)
    {
        const auto received = channel.Receive(5000);
        VERIFY_IS_TRUE(received.Succeeded());
        const auto decoded = DecodeControlResponse(received.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        return decoded.Value;
    }

    inline ControlResponse Request(
        RuntimeControlChannel& channel,
        const std::string_view requestId,
        const ControlMethod method,
        std::string payload = "{}")
    {
        SendRequest(channel, requestId, method, std::move(payload));
        return ReceiveResponse(channel);
    }

    template<typename TPredicate>
    bool WaitUntil(TPredicate&& predicate, const uint32_t timeoutMilliseconds = 3000)
    {
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        while (GetTickCount64() < deadline)
        {
            if (predicate())
            {
                return true;
            }
            Sleep(10);
        }
        return predicate();
    }
}
