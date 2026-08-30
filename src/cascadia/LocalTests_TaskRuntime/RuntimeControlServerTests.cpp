// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeControlTestFixtures.h"

#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::ControlFixtures;

namespace TaskRuntimeLocalTests
{
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
        TEST_METHOD(StopDuringSubscriptionPlanningCancelsBeforeRoute);
        TEST_METHOD(InvalidServerConfigurationFailsClosed);
        TEST_METHOD(StopCancelsIdleAcceptorAndClientReads);
        TEST_METHOD(StopFromRouterWorkerDoesNotJoinItself);
        TEST_METHOD(RapidDisconnectDuringSessionPublicationIsReapedSafely);
    };

    void RuntimeControlServerTests::RoutesTwoHelloValidatedClientsIndependently()
    {
        RunningControlServer server;
        auto first = ConnectAndHello(server, ClientOneId);
        auto second = ConnectAndHello(server, ClientTwoId);

        const auto firstResponse = Request(
            first,
            "30000000-0000-4000-8000-000000000031",
            ControlMethod::QueryTask);
        const auto secondResponse = Request(
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

        auto healthy = ConnectAndHello(server, ClientTwoId);
        const auto response = Request(
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
        auto client = ConnectAndHello(
            server,
            ClientOneId,
            static_cast<uint64_t>(ProtocolCapability::TaskQueries));
        const auto rejected = Request(
            client,
            "30000000-0000-4000-8000-000000000033",
            ControlMethod::SubmitCommand);
        VERIFY_IS_FALSE(rejected.Succeeded);
        VERIFY_IS_TRUE(rejected.Error.has_value());
        VERIFY_IS_TRUE(rejected.Error->Code == ProtocolRpcErrorCode::CapabilityRequired);
        VERIFY_ARE_EQUAL(size_t{ 0 }, server.Router->Calls.load(std::memory_order_relaxed));

        const auto ping = Request(
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
        VERIFY_IS_TRUE(WaitUntil([&] {
            return server.Server.Statistics().MalformedConnections == 1;
        }));
        VERIFY_ARE_EQUAL(size_t{ 0 }, server.Router->Calls.load(std::memory_order_relaxed));
    }

    void RuntimeControlServerTests::ConvertsRouterExceptionToTypedError()
    {
        RunningControlServer server;
        auto client = ConnectAndHello(server, ClientOneId);
        const auto failed = Request(
            client,
            "30000000-0000-4000-8000-000000000036",
            ControlMethod::Ping,
            "{\"throw\":true}");
        VERIFY_IS_FALSE(failed.Succeeded);
        VERIFY_IS_TRUE(failed.Error.has_value());
        VERIFY_IS_TRUE(failed.Error->Code == ProtocolRpcErrorCode::InternalError);

        const auto recovered = Request(
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
        auto slow = ConnectAndHello(server, ClientOneId);
        auto fast = ConnectAndHello(server, ClientTwoId);

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

        const auto fastResponse = Request(
            fast,
            "30000000-0000-4000-8000-000000000045",
            ControlMethod::Ping);
        VERIFY_IS_TRUE(fastResponse.Succeeded);
        VERIFY_IS_TRUE(fastResponse.ResultJson.find(ClientTwoId) != std::string::npos);
        VERIFY_IS_TRUE(WaitUntil([&] {
            return server.Server.Statistics().SlowClientDisconnects == 1;
        }, 5000));
    }

    void RuntimeControlServerTests::ClientLimitRejectsOnlyTheNewConnection()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 50;
        configuration.MaximumClients = 1;
        RunningControlServer server{ configuration };
        auto first = ConnectAndHello(server, ClientOneId);

        auto second = ConnectRuntimeControlChannel(server.Identity, 5000);
        VERIFY_IS_TRUE(second.Succeeded());
        const auto rejected = PerformRuntimeClientHandshake(
            second.Channel,
            Hello(server.Identity, ClientTwoId),
            2000);
        VERIFY_IS_FALSE(rejected.Succeeded());
        VERIFY_IS_TRUE(WaitUntil([&] {
            return server.Server.Statistics().CapacityRejectedConnections == 1;
        }));

        const auto stillReady = Request(
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
        auto first = ConnectAndHello(server, ClientOneId);
        first.Close();
        VERIFY_IS_TRUE(WaitUntil([&] {
            return server.Server.Statistics().CompletedConnections == 1;
        }));
        VERIFY_IS_TRUE(server.Server.Running());

        auto reconnected = ConnectAndHello(server, ClientTwoId);
        const auto response = Request(
            reconnected,
            "30000000-0000-4000-8000-000000000047",
            ControlMethod::Ping);
        VERIFY_IS_TRUE(response.Succeeded);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, server.Server.Statistics().AcceptedConnections);
    }

    void RuntimeControlServerTests::StopDuringSubscriptionPlanningCancelsBeforeRoute()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 60000;
        configuration.ReadTimeoutMilliseconds = 60000;
        RunningControlServer server{ configuration };
        const auto plannerEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const auto stopCompleted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY_IS_NOT_NULL(plannerEntered);
        VERIFY_IS_NOT_NULL(stopCompleted);
        std::atomic_bool plannerObservedCancellation{};
        server.Router->SetSubscriptionPlanner(
            [plannerEntered, &plannerObservedCancellation](
                const RuntimeControlClientContext&,
                const ControlRequest&,
                const RuntimeControlCancellationToken& cancellation) {
                SetEvent(plannerEntered);
                const auto deadline = GetTickCount64() + 2000;
                while (!cancellation.IsCancellationRequested() && GetTickCount64() < deadline)
                {
                    SwitchToThread();
                }
                plannerObservedCancellation.store(
                    cancellation.IsCancellationRequested(),
                    std::memory_order_release);
                return std::optional{ RuntimeControlSubscriptionPlan{
                    "40000000-0000-4000-8000-000000000061",
                    1,
                } };
            });

        auto client = ConnectAndHello(server, ClientOneId);
        ControlRequest request;
        request.RequestId = "30000000-0000-4000-8000-000000000061";
        request.Method = ControlMethod::SnapshotAndSubscribe;
        const auto encoded = EncodeControlRequest(request);
        VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_IS_TRUE(client.Send(encoded.Payload, 2000).Succeeded());
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(plannerEntered, 2000));

        uint64_t stopElapsed{};
        std::thread stopThread{ [&] {
            const auto started = GetTickCount64();
            server.Server.Stop();
            stopElapsed = GetTickCount64() - started;
            SetEvent(stopCompleted);
        } };
        const auto stopWait = WaitForSingleObject(stopCompleted, 2000);
        if (stopWait != WAIT_OBJECT_0)
        {
            client.Close();
        }
        stopThread.join();

        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, stopWait);
        VERIFY_IS_TRUE(stopElapsed < 2000);
        VERIFY_IS_TRUE(plannerObservedCancellation.load(std::memory_order_acquire));
        VERIFY_ARE_EQUAL(size_t{ 0 }, server.Router->Calls.load(std::memory_order_relaxed));
        VERIFY_ARE_EQUAL(size_t{ 1 }, server.Router->CancelledPlans.load(std::memory_order_relaxed));
        CloseHandle(plannerEntered);
        CloseHandle(stopCompleted);
    }

    void RuntimeControlServerTests::InvalidServerConfigurationFailsClosed()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto security = RequireSecurity(identity.UserSid);
        auto listener = CreateRuntimeControlPipe(identity, security.Attributes(), true);
        VERIFY_IS_TRUE(listener.Created());
        RuntimeControlServerConfiguration configuration;
        configuration.Handshake = RuntimeConfiguration(identity);
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
        const auto stopped = server.Server.StoppedHandle();
        VERIFY_IS_NOT_NULL(stopped);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_TIMEOUT }, WaitForSingleObject(stopped, 0));
        auto client = ConnectAndHello(server, ClientOneId);
        const auto started = GetTickCount64();
        server.Server.Stop();
        const auto elapsed = GetTickCount64() - started;
        VERIFY_IS_FALSE(server.Server.Running());
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(stopped, 0));
        VERIFY_IS_TRUE(
            server.Server.TerminalStatus() == RuntimeControlServerTerminalStatus::Stopped);
        VERIFY_IS_TRUE(elapsed < 2000);
        VERIFY_IS_FALSE(client.Receive(1000).Succeeded());
        server.Server.Stop();
    }

    void RuntimeControlServerTests::StopFromRouterWorkerDoesNotJoinItself()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 60000;
        configuration.ReadTimeoutMilliseconds = 60000;
        RunningControlServer server{ configuration };
        server.Router->SetHandler(
            [&](const RuntimeControlClientContext&,
                const ControlRequest&,
                const RuntimeControlCancellationToken&) {
                server.Server.Stop();
                RuntimeControlRouteResult result;
                result.Succeeded = true;
                return result;
            });

        auto client = ConnectAndHello(server, ClientOneId);
        SendRequest(
            client,
            "30000000-0000-4000-8000-000000000071",
            ControlMethod::Ping);
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(server.Server.StoppedHandle(), 2000));

        // The test thread is the external owner that performs the final join.
        server.Server.Stop();
        VERIFY_IS_FALSE(server.Server.Running());
        VERIFY_IS_TRUE(
            server.Server.TerminalStatus() == RuntimeControlServerTerminalStatus::Stopped);
        const auto possibleResponse = client.Receive(1000);
        const auto disconnected = possibleResponse.Succeeded() ?
                                      client.Receive(1000) :
                                      possibleResponse;
        VERIFY_IS_TRUE(disconnected.Status == RuntimeTransportStatus::Disconnected);
    }

    void RuntimeControlServerTests::RapidDisconnectDuringSessionPublicationIsReapedSafely()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 60000;
        RunningControlServer server{ configuration };
        constexpr uint64_t connectionCount{ 32 };
        for (uint64_t index = 0; index < connectionCount; ++index)
        {
            auto connected = ConnectRuntimeControlChannel(server.Identity, 5000);
            VERIFY_IS_TRUE(connected.Succeeded());
            connected.Channel.Close();
            VERIFY_IS_TRUE(WaitUntil([&] {
                return server.Server.Statistics().CompletedConnections >= index + 1;
            }, 5000));
        }

        VERIFY_IS_TRUE(server.Server.Running());
        auto healthy = ConnectAndHello(server, ClientTwoId);
        const auto response = Request(
            healthy,
            "30000000-0000-4000-8000-000000000072",
            ControlMethod::Ping);
        VERIFY_IS_TRUE(response.Succeeded);
    }
}
