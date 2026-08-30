// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeControlChannel.h"
#include "RuntimeControlHandshake.h"
#include "TaskRuntimeTestFixtures.h"

#include "ProtocolJson.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        struct ChannelPair
        {
            RuntimeControlChannel Server;
            RuntimeControlChannel Client;
            RuntimeIdentity Identity;
        };

        ChannelPair _channelPair()
        {
            UniqueRuntimeRoot root;
            auto identity = RequireIdentity(root.Path());
            auto security = RequireSecurity(identity.UserSid);
            auto listener = CreateRuntimeControlPipe(identity, security.Attributes(), true);
            VERIFY_IS_TRUE(listener.Created());

            RuntimeControlChannelResult accepted;
            std::thread acceptThread{ [&] {
                accepted = AcceptRuntimeControlChannel(listener.Pipe, 5000);
            } };
            auto connected = ConnectRuntimeControlChannel(identity, 5000);
            acceptThread.join();
            VERIFY_IS_TRUE(accepted.Succeeded());
            VERIFY_IS_TRUE(connected.Succeeded());
            return {
                std::move(accepted.Channel),
                std::move(connected.Channel),
                std::move(identity),
            };
        }

        HelloRequest _hello(const RuntimeIdentity& identity)
        {
            HelloRequest request;
            request.ClientInstanceId = "10000000-0000-4000-8000-000000000001";
            request.ExpectedRuntimeIdentity = identity.ProtocolIdentity;
            request.RequiredCapabilities = static_cast<uint64_t>(ProtocolCapability::TaskQueries);
            request.OptionalCapabilities = static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe);
            request.MaximumAcceptedControlFrameBytes = 32 * 1024;
            return request;
        }

        RuntimeHandshakeConfiguration _runtime(const RuntimeIdentity& identity)
        {
            RuntimeHandshakeConfiguration runtime;
            runtime.RuntimeIdentity = identity.ProtocolIdentity;
            runtime.RuntimeInstanceId = "20000000-0000-4000-8000-000000000002";
            runtime.Capabilities =
                static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe);
            runtime.MaximumControlFrameBytes = 64 * 1024;
            runtime.MaximumTerminalFrameBytes = 64 * 1024;
            runtime.HeartbeatMilliseconds = 1000;
            return runtime;
        }

        void _writeRaw(const HANDLE handle, const std::span<const uint8_t> bytes)
        {
            const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            VERIFY_IS_NOT_NULL(event);
            OVERLAPPED operation{};
            operation.hEvent = event;
            DWORD transferred{};
            const auto started = WriteFile(
                handle,
                bytes.data(),
                static_cast<DWORD>(bytes.size()),
                &transferred,
                &operation);
            if (!started)
            {
                VERIFY_ARE_EQUAL(DWORD{ ERROR_IO_PENDING }, GetLastError());
                VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(event, 5000));
                VERIFY_IS_TRUE(GetOverlappedResult(handle, &operation, &transferred, FALSE));
            }
            VERIFY_ARE_EQUAL(static_cast<DWORD>(bytes.size()), transferred);
            CloseHandle(event);
        }
    }

    class RuntimeControlChannelTests
    {
        TEST_CLASS(RuntimeControlChannelTests);

        TEST_METHOD(OverlappedChannelRoundTripsQueuedFrames);
        TEST_METHOD(ClientAndServerPerformValidatedHelloHandshake);
        TEST_METHOD(ClientRejectsWrongRuntimeHelloIdentity);
        TEST_METHOD(ReceiveTimesOutThenObservesPeerDisconnect);
        TEST_METHOD(MalformedRawFrameFailsDecoderClosed);
        TEST_METHOD(ConnectToMissingRuntimeIsBounded);
    };

    void RuntimeControlChannelTests::OverlappedChannelRoundTripsQueuedFrames()
    {
        auto pair = _channelPair();
        VERIFY_IS_TRUE(pair.Client.Send("{\"value\":1}", 2000).Succeeded());
        VERIFY_IS_TRUE(pair.Client.Send("{\"value\":2}", 2000).Succeeded());
        auto first = pair.Server.Receive(2000);
        auto second = pair.Server.Receive(2000);
        VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_IS_TRUE(second.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "{\"value\":1}" }, first.Payload);
        VERIFY_ARE_EQUAL(std::string{ "{\"value\":2}" }, second.Payload);
        VERIFY_IS_TRUE(pair.Server.Send("{\"ok\":true}", 2000).Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "{\"ok\":true}" }, pair.Client.Receive(2000).Payload);
    }

    void RuntimeControlChannelTests::ClientAndServerPerformValidatedHelloHandshake()
    {
        auto pair = _channelPair();
        RuntimeServerHandshakeResult server;
        const auto runtime = _runtime(pair.Identity);
        std::thread serverThread{ [&] {
            server = PerformRuntimeServerHandshake(pair.Server, runtime, 5000);
        } };
        const auto client = PerformRuntimeClientHandshake(pair.Client, _hello(pair.Identity), 5000);
        serverThread.join();
        VERIFY_IS_TRUE(server.Succeeded());
        VERIFY_IS_TRUE(server.Decision.Accepted);
        VERIFY_IS_TRUE(client.Succeeded());
        VERIFY_IS_TRUE(client.HandshakeError == HandshakeError::None);
        VERIFY_ARE_EQUAL(uint32_t{ 32 * 1024 }, client.Response.MaximumControlFrameBytes);
        VERIFY_IS_TRUE(pair.Client.Send("{}", 2000).Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "{}" }, pair.Server.Receive(2000).Payload);
    }

    void RuntimeControlChannelTests::ClientRejectsWrongRuntimeHelloIdentity()
    {
        auto pair = _channelPair();
        std::thread serverThread{ [&] {
            const auto requestPayload = pair.Server.Receive(5000);
            VERIFY_IS_TRUE(requestPayload.Succeeded());
            VERIFY_IS_TRUE(DecodeHelloRequest(requestPayload.Payload).Succeeded());
            HelloResponse response;
            response.RuntimeIdentity = "wrong-runtime:user";
            response.RuntimeInstanceId = "20000000-0000-4000-8000-000000000002";
            response.ProtocolMajor = 1;
            response.ProtocolMinor = 0;
            response.Capabilities = static_cast<uint64_t>(ProtocolCapability::TaskQueries);
            response.MaximumControlFrameBytes = 32 * 1024;
            response.MaximumTerminalFrameBytes = 64 * 1024;
            response.HeartbeatMilliseconds = 1000;
            const auto encoded = EncodeHelloResponse(response);
            VERIFY_IS_TRUE(encoded.Succeeded);
            VERIFY_IS_TRUE(pair.Server.Send(encoded.Payload, 5000).Succeeded());
        } };
        const auto client = PerformRuntimeClientHandshake(pair.Client, _hello(pair.Identity), 5000);
        serverThread.join();
        VERIFY_IS_FALSE(client.Succeeded());
        VERIFY_IS_TRUE(client.Status == RuntimeTransportStatus::ProtocolRejected);
        VERIFY_IS_TRUE(client.HandshakeError == HandshakeError::RuntimeIdentityMismatch);
    }

    void RuntimeControlChannelTests::ReceiveTimesOutThenObservesPeerDisconnect()
    {
        auto pair = _channelPair();
        const auto timeout = pair.Client.Receive(25);
        VERIFY_IS_TRUE(timeout.Status == RuntimeTransportStatus::Timeout);
        pair.Server.Close();
        const auto disconnected = pair.Client.Receive(2000);
        VERIFY_IS_TRUE(disconnected.Status == RuntimeTransportStatus::Disconnected);
    }

    void RuntimeControlChannelTests::MalformedRawFrameFailsDecoderClosed()
    {
        auto pair = _channelPair();
        const std::array<uint8_t, 4> emptyControlFrame{};
        _writeRaw(pair.Server.Handle(), emptyControlFrame);
        const auto malformed = pair.Client.Receive(2000);
        VERIFY_IS_TRUE(malformed.Status == RuntimeTransportStatus::MalformedFrame);
        VERIFY_IS_TRUE(pair.Client.Receive(2000).Status == RuntimeTransportStatus::MalformedFrame);
    }

    void RuntimeControlChannelTests::ConnectToMissingRuntimeIsBounded()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        const auto started = GetTickCount64();
        const auto result = ConnectRuntimeControlChannel(identity, 50);
        const auto elapsed = GetTickCount64() - started;
        VERIFY_IS_FALSE(result.Succeeded());
        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::Timeout);
        VERIFY_IS_TRUE(elapsed < 1000);
    }
}
