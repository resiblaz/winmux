// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeControlChannel.h"
#include "RuntimeControlConnector.h"
#include "RuntimeControlHandshake.h"
#include "RuntimeControlTestFixtures.h"

#include "ProtocolJson.h"

#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::ControlFixtures;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        class TestEvent final
        {
        public:
            explicit TestEvent(const bool signaled = false) :
                _handle{ CreateEventW(nullptr, TRUE, signaled, nullptr) }
            {
                VERIFY_IS_NOT_NULL(_handle);
            }

            ~TestEvent()
            {
                if (_handle)
                {
                    CloseHandle(_handle);
                }
            }

            TestEvent(const TestEvent&) = delete;
            TestEvent& operator=(const TestEvent&) = delete;

            [[nodiscard]] HANDLE Get() const noexcept
            {
                return _handle;
            }

            void Signal() const
            {
                VERIFY_IS_TRUE(SetEvent(_handle));
            }

        private:
            HANDLE _handle{};
        };

        struct ControlEndpoint
        {
            RuntimeIdentity Identity;
            RuntimeNamedPipe Listener;
        };

        struct ConnectedPair
        {
            RuntimeControlConnectionResult Client;
            RuntimeControlChannel Server;
            RuntimeServerHandshakeResult ServerHandshake;
        };

        ControlEndpoint _endpoint()
        {
            UniqueRuntimeRoot root;
            auto identity = RequireIdentity(root.Path());
            auto security = RequireSecurity(identity.UserSid);
            auto listener = CreateRuntimeControlPipe(identity, security.Attributes(), true);
            VERIFY_IS_TRUE(listener.Created());
            return {
                std::move(identity),
                std::move(listener.Pipe),
            };
        }

        ConnectedPair _connectEndpoint(
            ControlEndpoint& endpoint,
            RuntimeNamedPipeControlConnector& connector,
            const uint32_t clientPayloadLimit = 4096)
        {
            RuntimeControlChannel serverChannel;
            RuntimeServerHandshakeResult serverHandshake;
            std::thread serverThread{ [&] {
                auto accepted = AcceptRuntimeControlChannel(endpoint.Listener, 2000);
                if (!accepted.Succeeded())
                {
                    return;
                }
                serverHandshake = PerformRuntimeServerHandshake(
                    accepted.Channel,
                    RuntimeConfiguration(endpoint.Identity),
                    2000);
                serverChannel = std::move(accepted.Channel);
            } };

            auto client = connector.Connect(
                endpoint.Identity,
                Hello(
                    endpoint.Identity,
                    ClientOneId,
                    AllProtocolCapabilities,
                    clientPayloadLimit),
                2000);
            serverThread.join();
            return {
                std::move(client),
                std::move(serverChannel),
                std::move(serverHandshake),
            };
        }

        ConnectedPair _connectedPair(const uint32_t clientPayloadLimit = 4096)
        {
            auto endpoint = _endpoint();
            RuntimeNamedPipeControlConnector connector;
            return _connectEndpoint(endpoint, connector, clientPayloadLimit);
        }

        void _receiveHello(RuntimeControlChannel& channel)
        {
            const auto received = channel.Receive(2000);
            VERIFY_IS_TRUE(received.Succeeded());
            VERIFY_IS_TRUE(DecodeHelloRequest(received.Payload).Succeeded());
        }
    }

    class RuntimeControlConnectorTests
    {
        TEST_CLASS(RuntimeControlConnectorTests);

        TEST_METHOD(ConnectsPerformsHelloAndAppliesNegotiatedLimit);
        TEST_METHOD(MissingPipeHonorsBoundedTimeout);
        TEST_METHOD(PrecancelledAndWaitingConnectReturnCancelled);
        TEST_METHOD(CancelsHandshakeWaitWithoutReturningConnection);
        TEST_METHOD(ProtocolRejectedAndMalformedHelloReturnNoConnection);
        TEST_METHOD(CancelPendingIoTerminatesEpochAndReconnectsCleanly);
        TEST_METHOD(HandshakeUsesOneBoundedDeadline);
    };

    void RuntimeControlConnectorTests::ConnectsPerformsHelloAndAppliesNegotiatedLimit()
    {
        constexpr uint32_t clientPayloadLimit{ 4096 };
        auto pair = _connectedPair(clientPayloadLimit);
        VERIFY_IS_TRUE(pair.ServerHandshake.Succeeded());
        VERIFY_IS_TRUE(pair.ServerHandshake.Decision.Accepted);
        VERIFY_IS_TRUE(pair.Client.Succeeded());
        VERIFY_IS_NOT_NULL(pair.Client.Connection.get());
        VERIFY_IS_TRUE(pair.Client.Phase == RuntimeControlConnectPhase::Connected);
        VERIFY_IS_TRUE(pair.Client.HandshakeError == HandshakeError::None);
        VERIFY_ARE_EQUAL(clientPayloadLimit, pair.Client.Response.MaximumControlFrameBytes);

        const std::string oversized(clientPayloadLimit + 1, 'x');
        VERIFY_IS_TRUE(pair.Client.Connection->Send(oversized, 1000).Status ==
                       RuntimeTransportStatus::InvalidConfiguration);
        VERIFY_IS_TRUE(pair.Client.Connection->Send("{}", 1000).Succeeded());
        const auto received = pair.Server.Receive(1000);
        VERIFY_IS_TRUE(received.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "{}" }, received.Payload);
    }

    void RuntimeControlConnectorTests::MissingPipeHonorsBoundedTimeout()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        RuntimeNamedPipeControlConnector connector;
        const auto started = GetTickCount64();
        const auto result = connector.Connect(
            identity,
            Hello(identity, ClientOneId),
            75);
        const auto elapsed = GetTickCount64() - started;

        VERIFY_IS_FALSE(result.Succeeded());
        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::Timeout);
        VERIFY_IS_TRUE(result.Phase == RuntimeControlConnectPhase::Connecting);
        VERIFY_IS_NULL(result.Connection.get());
        VERIFY_IS_TRUE(elapsed < 1000);
    }

    void RuntimeControlConnectorTests::PrecancelledAndWaitingConnectReturnCancelled()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        RuntimeNamedPipeControlConnector connector;

        TestEvent precancelled{ true };
        const auto immediateStarted = GetTickCount64();
        const auto immediate = connector.Connect(
            identity,
            Hello(identity, ClientOneId),
            5000,
            precancelled.Get());
        VERIFY_IS_TRUE(immediate.Status == RuntimeTransportStatus::Cancelled);
        VERIFY_IS_TRUE(immediate.Phase == RuntimeControlConnectPhase::Connecting);
        VERIFY_IS_NULL(immediate.Connection.get());
        VERIFY_IS_TRUE(GetTickCount64() - immediateStarted < 1000);

        TestEvent started;
        TestEvent cancel;
        TestEvent completed;
        RuntimeControlConnectionResult waiting;
        std::thread connectThread{ [&] {
            started.Signal();
            waiting = connector.Connect(
                identity,
                Hello(identity, ClientTwoId),
                5000,
                cancel.Get());
            completed.Signal();
        } };
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(started.Get(), 1000));
        Sleep(25);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_TIMEOUT }, WaitForSingleObject(completed.Get(), 0));
        const auto cancelStarted = GetTickCount64();
        cancel.Signal();
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(completed.Get(), 1000));
        connectThread.join();

        VERIFY_IS_TRUE(waiting.Status == RuntimeTransportStatus::Cancelled);
        VERIFY_IS_TRUE(waiting.Phase == RuntimeControlConnectPhase::Connecting);
        VERIFY_IS_NULL(waiting.Connection.get());
        VERIFY_IS_TRUE(GetTickCount64() - cancelStarted < 1000);
    }

    void RuntimeControlConnectorTests::CancelsHandshakeWaitWithoutReturningConnection()
    {
        auto endpoint = _endpoint();
        TestEvent helloReceived;
        TestEvent releaseServer;
        TestEvent cancel;
        TestEvent connectCompleted;
        std::thread serverThread{ [&] {
            auto accepted = AcceptRuntimeControlChannel(endpoint.Listener, 2000);
            if (!accepted.Succeeded())
            {
                return;
            }
            _receiveHello(accepted.Channel);
            helloReceived.Signal();
            WaitForSingleObject(releaseServer.Get(), 2000);
        } };

        RuntimeNamedPipeControlConnector connector;
        RuntimeControlConnectionResult result;
        std::thread connectThread{ [&] {
            result = connector.Connect(
                endpoint.Identity,
                Hello(endpoint.Identity, ClientOneId),
                5000,
                cancel.Get());
            connectCompleted.Signal();
        } };

        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(helloReceived.Get(), 2000));
        cancel.Signal();
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connectCompleted.Get(), 1000));
        releaseServer.Signal();
        connectThread.join();
        serverThread.join();

        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::Cancelled);
        VERIFY_IS_TRUE(result.Phase == RuntimeControlConnectPhase::Handshaking);
        VERIFY_IS_NULL(result.Connection.get());
    }

    void RuntimeControlConnectorTests::ProtocolRejectedAndMalformedHelloReturnNoConnection()
    {
        {
            auto endpoint = _endpoint();
            const auto request = Hello(endpoint.Identity, ClientOneId);
            std::thread serverThread{ [&] {
                auto accepted = AcceptRuntimeControlChannel(endpoint.Listener, 2000);
                if (!accepted.Succeeded())
                {
                    return;
                }
                const auto received = accepted.Channel.Receive(2000);
                VERIFY_IS_TRUE(received.Succeeded());
                const auto decoded = DecodeHelloRequest(received.Payload);
                VERIFY_IS_TRUE(decoded.Succeeded());
                auto decision = NegotiateHandshake(decoded.Value, RuntimeConfiguration(endpoint.Identity));
                VERIFY_IS_TRUE(decision.Accepted);
                decision.Response.RuntimeIdentity = "wrong-runtime:user";
                const auto encoded = EncodeHelloResponse(decision.Response);
                VERIFY_IS_TRUE(encoded.Succeeded);
                VERIFY_IS_TRUE(accepted.Channel.Send(encoded.Payload, 2000).Succeeded());
            } };

            RuntimeNamedPipeControlConnector connector;
            const auto result = connector.Connect(endpoint.Identity, request, 2000);
            serverThread.join();
            VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::ProtocolRejected);
            VERIFY_IS_TRUE(result.Phase == RuntimeControlConnectPhase::Handshaking);
            VERIFY_IS_TRUE(result.HandshakeError == HandshakeError::RuntimeIdentityMismatch);
            VERIFY_IS_NULL(result.Connection.get());
        }

        {
            auto endpoint = _endpoint();
            std::thread serverThread{ [&] {
                auto accepted = AcceptRuntimeControlChannel(endpoint.Listener, 2000);
                if (!accepted.Succeeded())
                {
                    return;
                }
                _receiveHello(accepted.Channel);
                VERIFY_IS_TRUE(accepted.Channel.Send("{\"type\":", 2000).Succeeded());
            } };

            RuntimeNamedPipeControlConnector connector;
            const auto result = connector.Connect(
                endpoint.Identity,
                Hello(endpoint.Identity, ClientTwoId),
                2000);
            serverThread.join();
            VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::MalformedFrame);
            VERIFY_IS_TRUE(result.Phase == RuntimeControlConnectPhase::Handshaking);
            VERIFY_IS_NULL(result.Connection.get());
        }
    }

    void RuntimeControlConnectorTests::CancelPendingIoTerminatesEpochAndReconnectsCleanly()
    {
        auto endpoint = _endpoint();
        RuntimeNamedPipeControlConnector connector;
        auto pair = _connectEndpoint(endpoint, connector);
        VERIFY_IS_TRUE(pair.Client.Succeeded());
        TestEvent receiveStarted;
        TestEvent receiveCompleted;
        RuntimeReceiveResult cancelled;
        std::thread reader{ [&] {
            receiveStarted.Signal();
            cancelled = pair.Client.Connection->Receive(5000);
            receiveCompleted.Signal();
        } };

        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(receiveStarted.Get(), 1000));
        pair.Client.Connection->CancelPendingIo();
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(receiveCompleted.Get(), 1000));
        reader.join();
        VERIFY_IS_TRUE(cancelled.Status == RuntimeTransportStatus::Cancelled);
        VERIFY_IS_FALSE(pair.Client.Connection->Connected());
        VERIFY_IS_TRUE(pair.Client.Connection->Receive(1000).Status ==
                       RuntimeTransportStatus::Cancelled);
        VERIFY_IS_TRUE(pair.Client.Connection->Send("{}", 1000).Status ==
                       RuntimeTransportStatus::Cancelled);

        pair.Client.Connection->Close();
        pair.Server.Close();
        auto security = RequireSecurity(endpoint.Identity.UserSid);
        auto listener = CreateRuntimeControlPipe(
            endpoint.Identity,
            security.Attributes(),
            true);
        VERIFY_IS_TRUE(listener.Created());
        endpoint.Listener = std::move(listener.Pipe);

        auto replacement = _connectEndpoint(endpoint, connector);
        VERIFY_IS_TRUE(replacement.Client.Succeeded());
        VERIFY_IS_TRUE(replacement.ServerHandshake.Succeeded());
    }

    void RuntimeControlConnectorTests::HandshakeUsesOneBoundedDeadline()
    {
        auto endpoint = _endpoint();
        TestEvent helloReceived;
        TestEvent releaseServer;
        std::thread serverThread{ [&] {
            auto accepted = AcceptRuntimeControlChannel(endpoint.Listener, 2000);
            if (!accepted.Succeeded())
            {
                return;
            }
            _receiveHello(accepted.Channel);
            helloReceived.Signal();
            WaitForSingleObject(releaseServer.Get(), 2000);
        } };

        RuntimeNamedPipeControlConnector connector;
        const auto started = GetTickCount64();
        const auto result = connector.Connect(
            endpoint.Identity,
            Hello(endpoint.Identity, ClientOneId),
            250);
        const auto elapsed = GetTickCount64() - started;
        releaseServer.Signal();
        serverThread.join();

        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(helloReceived.Get(), 0));
        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::Timeout);
        VERIFY_IS_TRUE(result.Phase == RuntimeControlConnectPhase::Handshaking);
        VERIFY_IS_NULL(result.Connection.get());
        VERIFY_IS_TRUE(elapsed < 1000);
    }
}
