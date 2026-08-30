// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimePipe.h"
#include "RuntimeTerminalChannel.h"
#include "RuntimeTerminalEndpoint.h"
#include "RuntimeTerminalHandshake.h"
#include "TaskRuntimeProcessWin32Support.h"
#include "TaskRuntimeTestFixtures.h"

#include "ProtocolCapabilities.h"
#include "ProtocolJson.h"

#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr std::string_view TerminalSessionId{
            "95000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view TerminalClientId{
            "95000000-0000-4000-8000-000000000002"
        };
        constexpr std::string_view RuntimeInstanceId{
            "95000000-0000-4000-8000-000000000003"
        };
        constexpr std::string_view OtherRuntimeInstanceId{
            "95000000-0000-4000-8000-000000000004"
        };
        constexpr uint64_t TerminalGeneration{ 9 };
        constexpr uint32_t TerminalPayloadBytes{ 64 * 1024 };

        struct HandshakePair
        {
            RuntimeTerminalChannel Server;
            RuntimeTerminalChannel Client;
            RuntimeIdentity Identity;
            RuntimeTerminalEndpoint Endpoint;
        };

        HandshakePair _pair()
        {
            UniqueRuntimeRoot root;
            auto identity = RequireIdentity(root.Path());
            const auto endpoint = BuildRuntimeTerminalEndpoint(
                identity,
                TerminalSessionId,
                TerminalGeneration);
            VERIFY_IS_TRUE(endpoint.Succeeded());
            auto security = RequireSecurity(identity.UserSid);
            auto listener = CreateRuntimeTerminalPipe(
                endpoint.Endpoint,
                security.Attributes());
            VERIFY_IS_TRUE(listener.Created());

            RuntimeTerminalChannelResult accepted;
            std::thread acceptThread{ [&] {
                accepted = AcceptRuntimeTerminalChannel(
                    listener.Pipe,
                    endpoint.Endpoint,
                    TerminalPayloadBytes,
                    5000);
            } };
            auto connected = ConnectRuntimeTerminalChannel(
                endpoint.Endpoint,
                TerminalPayloadBytes,
                5000);
            acceptThread.join();
            VERIFY_IS_TRUE(accepted.Succeeded());
            VERIFY_IS_TRUE(connected.Succeeded());
            return {
                std::move(accepted.Channel),
                std::move(connected.Channel),
                std::move(identity),
                endpoint.Endpoint,
            };
        }

        HelloRequest _request(const RuntimeIdentity& identity)
        {
            HelloRequest request;
            request.ClientInstanceId = std::string{ TerminalClientId };
            request.ClientKind = ProtocolClientKind::TestPeer;
            request.ExpectedRuntimeIdentity = identity.ProtocolIdentity;
            request.RequiredCapabilities =
                static_cast<uint64_t>(ProtocolCapability::TerminalStream);
            request.OptionalCapabilities =
                static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
            request.MaximumAcceptedControlFrameBytes = 32 * 1024;
            return request;
        }

        RuntimeHandshakeConfiguration _runtime(const RuntimeIdentity& identity)
        {
            RuntimeHandshakeConfiguration runtime;
            runtime.RuntimeIdentity = identity.ProtocolIdentity;
            runtime.RuntimeInstanceId = std::string{ RuntimeInstanceId };
            runtime.Capabilities =
                static_cast<uint64_t>(ProtocolCapability::TerminalStream) |
                static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
            runtime.MaximumControlFrameBytes = 64 * 1024;
            runtime.MaximumTerminalFrameBytes = TerminalPayloadBytes;
            runtime.HeartbeatMilliseconds = 1000;
            return runtime;
        }

        std::vector<uint8_t> _bytes(const std::string_view value)
        {
            std::vector<uint8_t> result;
            result.reserve(value.size());
            for (const auto character : value)
            {
                result.push_back(static_cast<uint8_t>(
                    static_cast<unsigned char>(character)));
            }
            return result;
        }

        TerminalFrame _helloFrame(const HelloRequest& request)
        {
            const auto encoded = EncodeHelloRequest(request);
            VERIFY_IS_TRUE(encoded.Succeeded);
            TerminalFrame frame;
            frame.Kind = TerminalFrameKind::Hello;
            frame.Sequence = 1;
            frame.Generation = 0;
            frame.Payload = _bytes(encoded.Payload);
            return frame;
        }

        TerminalFrame _helloResponseFrame(const HelloResponse& response)
        {
            const auto encoded = EncodeHelloResponse(response);
            VERIFY_IS_TRUE(encoded.Succeeded);
            TerminalFrame frame;
            frame.Kind = TerminalFrameKind::Hello;
            frame.Sequence = 1;
            frame.Generation = 0;
            frame.Payload = _bytes(encoded.Payload);
            return frame;
        }
    }

    class RuntimeTerminalHandshakeTests
    {
        TEST_CLASS(RuntimeTerminalHandshakeTests);

        TEST_METHOD(HandshakeBindsIdentityCapabilityAndPayloadLimit);
        TEST_METHOD(ClientRejectsWrongIdentityAndMissingTerminalCapability);
        TEST_METHOD(ServerRejectsWrongEndpointIdentityWithErrorFrame);
        TEST_METHOD(ServerRequiresHelloAsFirstSequenceOneFrame);
        TEST_METHOD(LocalHandshakeConfigurationFailsBeforeWire);
        TEST_METHOD(ClientDeadlineAndCancellationRemainBounded);
    };

    void RuntimeTerminalHandshakeTests::HandshakeBindsIdentityCapabilityAndPayloadLimit()
    {
        auto pair = _pair();
        RuntimeTerminalServerHandshakeResult server;
        const auto runtime = _runtime(pair.Identity);
        std::thread serverThread{ [&] {
            server = PerformRuntimeTerminalServerHandshake(
                pair.Server,
                runtime,
                5000);
        } };
        const auto client = PerformRuntimeTerminalClientHandshake(
            pair.Client,
            _request(pair.Identity),
            RuntimeInstanceId,
            5000);
        serverThread.join();

        VERIFY_IS_TRUE(server.Succeeded());
        VERIFY_IS_TRUE(server.Decision.Accepted);
        VERIFY_IS_TRUE(client.Succeeded());
        VERIFY_IS_TRUE(client.HandshakeError == HandshakeError::None);
        VERIFY_ARE_EQUAL(
            std::string{ RuntimeInstanceId },
            client.Response.RuntimeInstanceId);
        VERIFY_ARE_EQUAL(
            TerminalPayloadBytes,
            client.Response.MaximumTerminalFrameBytes);
        VERIFY_IS_TRUE(HasCapability(
            client.Response.Capabilities,
            ProtocolCapability::TerminalStream));

        TerminalFrame attach;
        attach.Kind = TerminalFrameKind::Attach;
        attach.Sequence = 2;
        attach.Generation = TerminalGeneration;
        VERIFY_IS_TRUE(pair.Server.Send(attach, 2000).Succeeded());
        const auto received = pair.Client.Receive(2000);
        VERIFY_IS_TRUE(received.Succeeded());
        VERIFY_IS_TRUE(received.Frame.Kind == TerminalFrameKind::Attach);
    }

    void RuntimeTerminalHandshakeTests::ClientRejectsWrongIdentityAndMissingTerminalCapability()
    {
        for (size_t test = 0; test < 3; ++test)
        {
            auto pair = _pair();
            UniqueRuntimeRoot otherRoot;
            const auto otherIdentity = RequireIdentity(otherRoot.Path());
            std::thread serverThread{ [&] {
                const auto request = pair.Server.Receive(5000);
                VERIFY_IS_TRUE(request.Succeeded());
                auto response = _runtime(pair.Identity);
                HelloResponse hello;
                hello.RuntimeIdentity = test == 0 ?
                                            otherIdentity.ProtocolIdentity :
                                            pair.Identity.ProtocolIdentity;
                hello.RuntimeInstanceId = test == 2 ?
                                              std::string{ OtherRuntimeInstanceId } :
                                              response.RuntimeInstanceId;
                hello.ProtocolMajor = CurrentProtocolMajor;
                hello.ProtocolMinor = CurrentProtocolMinor;
                hello.Capabilities = test != 1 ?
                                         response.Capabilities :
                                         static_cast<uint64_t>(
                                             ProtocolCapability::RuntimeLifecycle);
                hello.MaximumControlFrameBytes = response.MaximumControlFrameBytes;
                hello.MaximumTerminalFrameBytes = response.MaximumTerminalFrameBytes;
                hello.HeartbeatMilliseconds = response.HeartbeatMilliseconds;
                VERIFY_IS_TRUE(pair.Server.Send(
                    _helloResponseFrame(hello),
                    5000).Succeeded());
            } };
            const auto client = PerformRuntimeTerminalClientHandshake(
                pair.Client,
                _request(pair.Identity),
                RuntimeInstanceId,
                5000);
            serverThread.join();
            VERIFY_IS_TRUE(
                client.Status == RuntimeTransportStatus::ProtocolRejected);
        }
    }

    void RuntimeTerminalHandshakeTests::ServerRejectsWrongEndpointIdentityWithErrorFrame()
    {
        auto pair = _pair();
        UniqueRuntimeRoot otherRoot;
        const auto otherIdentity = RequireIdentity(otherRoot.Path());
        auto request = _request(otherIdentity);
        RuntimeTerminalServerHandshakeResult server;
        std::thread serverThread{ [&] {
            server = PerformRuntimeTerminalServerHandshake(
                pair.Server,
                _runtime(pair.Identity),
                5000);
        } };
        VERIFY_IS_TRUE(pair.Client.Send(_helloFrame(request), 5000).Succeeded());
        const auto rejection = pair.Client.Receive(5000);
        serverThread.join();

        VERIFY_IS_TRUE(
            server.Status == RuntimeTransportStatus::ProtocolRejected);
        VERIFY_IS_TRUE(rejection.Succeeded());
        VERIFY_IS_TRUE(rejection.Frame.Kind == TerminalFrameKind::Error);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, rejection.Frame.Sequence);
    }

    void RuntimeTerminalHandshakeTests::ServerRequiresHelloAsFirstSequenceOneFrame()
    {
        auto pair = _pair();
        RuntimeTerminalServerHandshakeResult server;
        std::thread serverThread{ [&] {
            server = PerformRuntimeTerminalServerHandshake(
                pair.Server,
                _runtime(pair.Identity),
                5000);
        } };
        TerminalFrame ping;
        ping.Kind = TerminalFrameKind::Ping;
        ping.Sequence = 1;
        ping.Generation = 0;
        VERIFY_IS_TRUE(pair.Client.Send(ping, 5000).Succeeded());
        const auto rejection = pair.Client.Receive(5000);
        serverThread.join();

        VERIFY_IS_TRUE(server.Status == RuntimeTransportStatus::MalformedFrame);
        VERIFY_IS_TRUE(rejection.Succeeded());
        VERIFY_IS_TRUE(rejection.Frame.Kind == TerminalFrameKind::Error);
    }

    void RuntimeTerminalHandshakeTests::LocalHandshakeConfigurationFailsBeforeWire()
    {
        {
            auto pair = _pair();
            auto request = _request(pair.Identity);
            request.RequiredCapabilities = 0;
            request.OptionalCapabilities |=
                static_cast<uint64_t>(ProtocolCapability::TerminalStream);
            const auto rejected = PerformRuntimeTerminalClientHandshake(
                pair.Client,
                request,
                RuntimeInstanceId,
                5000);
            VERIFY_IS_TRUE(
                rejected.Status == RuntimeTransportStatus::InvalidConfiguration);
            VERIFY_IS_TRUE(
                pair.Server.Receive(25).Status == RuntimeTransportStatus::Timeout);
        }
        {
            auto pair = _pair();
            auto runtime = _runtime(pair.Identity);
            runtime.Capabilities =
                static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
            const auto missingCapability = PerformRuntimeTerminalServerHandshake(
                pair.Server,
                runtime,
                5000);
            VERIFY_IS_TRUE(
                missingCapability.Status ==
                RuntimeTransportStatus::InvalidConfiguration);

            runtime = _runtime(pair.Identity);
            runtime.MaximumTerminalFrameBytes /= 2;
            const auto wrongPayload = PerformRuntimeTerminalServerHandshake(
                pair.Server,
                runtime,
                5000);
            VERIFY_IS_TRUE(
                wrongPayload.Status == RuntimeTransportStatus::InvalidConfiguration);
        }
    }

    void RuntimeTerminalHandshakeTests::ClientDeadlineAndCancellationRemainBounded()
    {
        {
            auto pair = _pair();
            const auto started = GetTickCount64();
            const auto timeout = PerformRuntimeTerminalClientHandshake(
                pair.Client,
                _request(pair.Identity),
                RuntimeInstanceId,
                50);
            VERIFY_IS_TRUE(timeout.Status == RuntimeTransportStatus::Timeout);
            VERIFY_IS_TRUE(GetTickCount64() - started < 1000);
        }
        {
            auto pair = _pair();
            ProcessFixtures::details::HandleOwner cancellation{
                CreateEventW(nullptr, TRUE, TRUE, nullptr)
            };
            const auto cancelled = PerformRuntimeTerminalClientHandshake(
                pair.Client,
                _request(pair.Identity),
                RuntimeInstanceId,
                5000,
                cancellation.Get());
            VERIFY_IS_TRUE(
                cancelled.Status == RuntimeTransportStatus::Cancelled);
        }
    }
}
