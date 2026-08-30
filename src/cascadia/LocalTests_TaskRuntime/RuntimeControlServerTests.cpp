// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeControlChannel.h"
#include "RuntimeControlServer.h"
#include "TaskRuntimeTestFixtures.h"

#include "ProtocolRpcJson.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr std::string_view ClientOneId{ "10000000-0000-4000-8000-000000000011" };
        constexpr std::string_view ClientTwoId{ "10000000-0000-4000-8000-000000000012" };

        class FakeControlRouter final : public IRuntimeControlRequestRouter
        {
        public:
            RuntimeControlRouteResult Route(
                const RuntimeControlClientContext& client,
                const ControlRequest& request,
                const RuntimeControlCancellationToken& cancellation) override
            {
                Calls.fetch_add(1, std::memory_order_relaxed);
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
        };

        RuntimeHandshakeConfiguration _runtimeConfiguration(const RuntimeIdentity& identity)
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

        HelloRequest _hello(
            const RuntimeIdentity& identity,
            const std::string_view clientId,
            const ProtocolCapabilitySet optionalCapabilities = AllProtocolCapabilities)
        {
            HelloRequest request;
            request.ClientInstanceId = clientId;
            request.ClientKind = ProtocolClientKind::TestPeer;
            request.ExpectedRuntimeIdentity = identity.ProtocolIdentity;
            request.OptionalCapabilities = optionalCapabilities;
            request.MaximumAcceptedControlFrameBytes = 256 * 1024;
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
                configuration.Handshake = _runtimeConfiguration(Identity);
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

        RuntimeControlChannel _connectAndHello(
            RunningControlServer& server,
            const std::string_view clientId,
            const ProtocolCapabilitySet optionalCapabilities = AllProtocolCapabilities)
        {
            auto connected = ConnectRuntimeControlChannel(server.Identity, 5000);
            VERIFY_IS_TRUE(connected.Succeeded());
            const auto handshake = PerformRuntimeClientHandshake(
                connected.Channel,
                _hello(server.Identity, clientId, optionalCapabilities),
                5000);
            VERIFY_IS_TRUE(handshake.Succeeded());
            return std::move(connected.Channel);
        }

        ControlResponse _request(
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
            const auto received = channel.Receive(5000);
            VERIFY_IS_TRUE(received.Succeeded());
            const auto decoded = DecodeControlResponse(received.Payload);
            VERIFY_IS_TRUE(decoded.Succeeded());
            return decoded.Value;
        }

        template<typename TPredicate>
        bool _waitUntil(TPredicate&& predicate, const uint32_t timeoutMilliseconds = 3000)
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

    class RuntimeControlServerTests
    {
        TEST_CLASS(RuntimeControlServerTests);

        TEST_METHOD(RoutesTwoHelloValidatedClientsIndependently);
        TEST_METHOD(StalledHelloDoesNotBlockAnotherClient);
        TEST_METHOD(RejectsCapabilityWithoutInvokingRouterAndKeepsSessionReady);
        TEST_METHOD(RejectsRequestBeforeHelloAndRecordsMalformedPeer);
        TEST_METHOD(ConvertsRouterExceptionToTypedError);
        TEST_METHOD(SlowClientDoesNotBlockAnotherClient);
        TEST_METHOD(ClientLimitRejectsOnlyTheNewConnection);
        TEST_METHOD(LastClientDisconnectLeavesServerAvailableForReconnect);
        TEST_METHOD(InvalidServerConfigurationFailsClosed);
        TEST_METHOD(StopCancelsIdleAcceptorAndClientReads);
    };

    void RuntimeControlServerTests::RoutesTwoHelloValidatedClientsIndependently()
    {
        RunningControlServer server;
        auto first = _connectAndHello(server, ClientOneId);
        auto second = _connectAndHello(server, ClientTwoId);

        const auto firstResponse = _request(
            first,
            "30000000-0000-4000-8000-000000000031",
            ControlMethod::QueryTask);
        const auto secondResponse = _request(
            second,
            "30000000-0000-4000-8000-000000000032",
            ControlMethod::RuntimeStatus);
        VERIFY_IS_TRUE(firstResponse.Succeeded);
        VERIFY_IS_TRUE(secondResponse.Succeeded);
        VERIFY_IS_TRUE(firstResponse.ResultJson.find(ClientOneId) != std::string::npos);
        VERIFY_IS_TRUE(secondResponse.ResultJson.find(ClientTwoId) != std::string::npos);
        VERIFY_ARE_EQUAL(size_t{ 2 }, server.Router->Calls.load(std::memory_order_relaxed));
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, server.Server.Statistics().AcceptedConnections);
    }

    void RuntimeControlServerTests::StalledHelloDoesNotBlockAnotherClient()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 50;
        configuration.HandshakeTimeoutMilliseconds = 1000;
        RunningControlServer server{ configuration };
        auto stalled = ConnectRuntimeControlChannel(server.Identity, 5000);
        VERIFY_IS_TRUE(stalled.Succeeded());

        auto healthy = _connectAndHello(server, ClientTwoId);
        const auto response = _request(
            healthy,
            "30000000-0000-4000-8000-000000000030",
            ControlMethod::Ping);
        VERIFY_IS_TRUE(response.Succeeded);
        VERIFY_IS_TRUE(response.ResultJson.find(ClientTwoId) != std::string::npos);
    }

    void RuntimeControlServerTests::RejectsCapabilityWithoutInvokingRouterAndKeepsSessionReady()
    {
        RuntimeControlServerConfiguration configuration;
        RunningControlServer server{
            configuration,
            static_cast<uint64_t>(ProtocolCapability::TaskQueries)
        };
        auto client = _connectAndHello(
            server,
            ClientOneId,
            static_cast<uint64_t>(ProtocolCapability::TaskQueries));
        const auto rejected = _request(
            client,
            "30000000-0000-4000-8000-000000000033",
            ControlMethod::SubmitCommand);
        VERIFY_IS_FALSE(rejected.Succeeded);
        VERIFY_IS_TRUE(rejected.Error.has_value());
        VERIFY_IS_TRUE(rejected.Error->Code == ProtocolRpcErrorCode::CapabilityRequired);
        VERIFY_ARE_EQUAL(size_t{ 0 }, server.Router->Calls.load(std::memory_order_relaxed));

        const auto ping = _request(
            client,
            "30000000-0000-4000-8000-000000000034",
            ControlMethod::Ping);
        VERIFY_IS_TRUE(ping.Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 1 }, server.Router->Calls.load(std::memory_order_relaxed));
    }

    void RuntimeControlServerTests::RejectsRequestBeforeHelloAndRecordsMalformedPeer()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 50;
        RunningControlServer server{ configuration };
        auto connected = ConnectRuntimeControlChannel(server.Identity, 5000);
        VERIFY_IS_TRUE(connected.Succeeded());

        ControlRequest request;
        request.RequestId = "30000000-0000-4000-8000-000000000035";
        const auto encoded = EncodeControlRequest(request);
        VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_IS_TRUE(connected.Channel.Send(encoded.Payload, 2000).Succeeded());
        const auto closed = connected.Channel.Receive(2000);
        VERIFY_IS_FALSE(closed.Succeeded());
        VERIFY_IS_TRUE(_waitUntil([&] {
            return server.Server.Statistics().MalformedConnections == 1;
        }));
        VERIFY_ARE_EQUAL(size_t{ 0 }, server.Router->Calls.load(std::memory_order_relaxed));
    }

    void RuntimeControlServerTests::ConvertsRouterExceptionToTypedError()
    {
        RunningControlServer server;
        auto client = _connectAndHello(server, ClientOneId);
        const auto failed = _request(
            client,
            "30000000-0000-4000-8000-000000000036",
            ControlMethod::Ping,
            "{\"throw\":true}");
        VERIFY_IS_FALSE(failed.Succeeded);
        VERIFY_IS_TRUE(failed.Error.has_value());
        VERIFY_IS_TRUE(failed.Error->Code == ProtocolRpcErrorCode::InternalError);

        const auto recovered = _request(
            client,
            "30000000-0000-4000-8000-000000000037",
            ControlMethod::Ping);
        VERIFY_IS_TRUE(recovered.Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 2 }, server.Router->Calls.load(std::memory_order_relaxed));
    }

    void RuntimeControlServerTests::SlowClientDoesNotBlockAnotherClient()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 50;
        configuration.WriteTimeoutMilliseconds = 1000;
        configuration.MaximumQueuedMessages = 1;
        configuration.MaximumQueuedBytes = 512 * 1024;
        RunningControlServer server{ configuration };
        auto slow = _connectAndHello(server, ClientOneId);
        auto fast = _connectAndHello(server, ClientTwoId);

        const std::array<std::string_view, 4> requestIds{
            "30000000-0000-4000-8000-000000000041",
            "30000000-0000-4000-8000-000000000042",
            "30000000-0000-4000-8000-000000000043",
            "30000000-0000-4000-8000-000000000044",
        };
        for (const auto requestId : requestIds)
        {
            ControlRequest request;
            request.RequestId = requestId;
            request.PayloadJson = "{\"large\":true}";
            const auto encoded = EncodeControlRequest(request);
            VERIFY_IS_TRUE(encoded.Succeeded);
            if (!slow.Send(encoded.Payload, 2000).Succeeded())
            {
                break;
            }
        }

        const auto fastResponse = _request(
            fast,
            "30000000-0000-4000-8000-000000000045",
            ControlMethod::Ping);
        VERIFY_IS_TRUE(fastResponse.Succeeded);
        VERIFY_IS_TRUE(fastResponse.ResultJson.find(ClientTwoId) != std::string::npos);
        VERIFY_IS_TRUE(_waitUntil([&] {
            return server.Server.Statistics().SlowClientDisconnects == 1;
        }, 5000));
    }

    void RuntimeControlServerTests::ClientLimitRejectsOnlyTheNewConnection()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 50;
        configuration.MaximumClients = 1;
        RunningControlServer server{ configuration };
        auto first = _connectAndHello(server, ClientOneId);

        auto second = ConnectRuntimeControlChannel(server.Identity, 5000);
        VERIFY_IS_TRUE(second.Succeeded());
        const auto rejected = PerformRuntimeClientHandshake(
            second.Channel,
            _hello(server.Identity, ClientTwoId),
            2000);
        VERIFY_IS_FALSE(rejected.Succeeded());
        VERIFY_IS_TRUE(_waitUntil([&] {
            return server.Server.Statistics().CapacityRejectedConnections == 1;
        }));

        const auto stillReady = _request(
            first,
            "30000000-0000-4000-8000-000000000046",
            ControlMethod::Ping);
        VERIFY_IS_TRUE(stillReady.Succeeded);
    }

    void RuntimeControlServerTests::LastClientDisconnectLeavesServerAvailableForReconnect()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 50;
        RunningControlServer server{ configuration };
        auto first = _connectAndHello(server, ClientOneId);
        first.Close();
        VERIFY_IS_TRUE(_waitUntil([&] {
            return server.Server.Statistics().CompletedConnections == 1;
        }));
        VERIFY_IS_TRUE(server.Server.Running());

        auto reconnected = _connectAndHello(server, ClientTwoId);
        const auto response = _request(
            reconnected,
            "30000000-0000-4000-8000-000000000047",
            ControlMethod::Ping);
        VERIFY_IS_TRUE(response.Succeeded);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, server.Server.Statistics().AcceptedConnections);
    }

    void RuntimeControlServerTests::InvalidServerConfigurationFailsClosed()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto security = RequireSecurity(identity.UserSid);
        auto listener = CreateRuntimeControlPipe(identity, security.Attributes(), true);
        VERIFY_IS_TRUE(listener.Created());
        RuntimeControlServerConfiguration configuration;
        configuration.Handshake = _runtimeConfiguration(identity);
        configuration.MaximumClients = 0;
        const auto result = StartRuntimeControlServer(
            identity,
            std::move(security),
            std::move(listener.Pipe),
            std::move(configuration),
            std::make_shared<FakeControlRouter>());
        VERIFY_IS_FALSE(result.Started());
        VERIFY_IS_FALSE(static_cast<bool>(result.Server));
        VERIFY_IS_TRUE(result.Status == RuntimeControlServerStatus::InvalidConfiguration);
    }

    void RuntimeControlServerTests::StopCancelsIdleAcceptorAndClientReads()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 60000;
        configuration.ReadTimeoutMilliseconds = 60000;
        RunningControlServer server{ configuration };
        auto client = _connectAndHello(server, ClientOneId);
        const auto started = GetTickCount64();
        server.Server.Stop();
        const auto elapsed = GetTickCount64() - started;
        VERIFY_IS_FALSE(server.Server.Running());
        VERIFY_IS_TRUE(elapsed < 2000);
        VERIFY_IS_FALSE(client.Receive(1000).Succeeded());
    }
}
