// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeProcessTestFixtures.h"

#include "RuntimeBootstrap.h"

#include "ProtocolCapabilities.h"
#include "ProtocolValidation.h"
#include "RuntimeFoundationRpc.h"
#include "TaskProjectionRpc.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::ProcessFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        void _verifyReady(TaskRuntimeProcess& process)
        {
            VERIFY_IS_TRUE(process.WaitUntilReady());
            VERIFY_IS_TRUE(process.Running());
            VERIFY_ARE_NOT_EQUAL(DWORD{ 0 }, process.ProcessId());
        }

        ControlResponse _requireSucceededResponse(
            RuntimeControlChannel& channel,
            const std::string_view requestId,
            const ControlMethod method)
        {
            const auto response = RequireProductResponse(channel, requestId, method);
            VERIFY_IS_TRUE(response.Succeeded);
            VERIFY_IS_FALSE(response.Error.has_value());
            return response;
        }

        void _verifyPing(
            RuntimeControlChannel& channel,
            const std::string_view requestId,
            const std::string_view runtimeInstanceId,
            const std::string_view clientInstanceId)
        {
            const auto response = _requireSucceededResponse(
                channel,
                requestId,
                ControlMethod::Ping);
            const auto decoded = DecodePingResult(response.ResultJson);
            VERIFY_IS_TRUE(decoded.Succeeded());
            VERIFY_ARE_EQUAL(std::string{ runtimeInstanceId }, decoded.Value.RuntimeInstanceId);
            VERIFY_ARE_EQUAL(std::string{ clientInstanceId }, decoded.Value.ClientInstanceId);
        }
    }

    class RuntimeProductHostProcessTests
    {
        TEST_CLASS(RuntimeProductHostProcessTests);

        TEST_METHOD(RealServeAcceptsHelloWithNegotiatedContract);
        TEST_METHOD(PingAndRuntimeStatusReportConcreteHost);
        TEST_METHOD(ReconnectPreservesRuntimeInstanceAndProcess);
        TEST_METHOD(SecondIdentityInstanceExitsThreeWithoutDisruptingOwner);
        TEST_METHOD(MalformedPeerIsIsolatedFromHealthyPeers);
        TEST_METHOD(ShutdownEventStopsGracefullyAndReleasesIdentityResources);
        TEST_METHOD(RealRuntimePersistsTerminalUiCreateAndQuery);
    };

    void RuntimeProductHostProcessTests::RealServeAcceptsHelloWithNegotiatedContract()
    {
        ProductRuntimeTestIdentity identity;
        TaskRuntimeProcess process{ identity };
        _verifyReady(process);

        auto client = RequireProductControlClient(
            identity,
            "71000000-0000-4000-8000-000000000001");
        VERIFY_IS_TRUE(client.Channel.Connected());
        VERIFY_ARE_EQUAL(identity.Identity().ProtocolIdentity, client.Handshake.RuntimeIdentity);
        VERIFY_IS_TRUE(IsCanonicalProtocolUuid(client.Handshake.RuntimeInstanceId));
        VERIFY_ARE_EQUAL(CurrentProtocolMajor, client.Handshake.ProtocolMajor);
        VERIFY_ARE_EQUAL(CurrentProtocolMinor, client.Handshake.ProtocolMinor);
        VERIFY_ARE_EQUAL(
            static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle) |
                static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                static_cast<uint64_t>(ProtocolCapability::TaskCommands),
            client.Handshake.Capabilities);
        VERIFY_IS_TRUE(client.Handshake.MaximumControlFrameBytes >= MinimumControlFrameBytes);
        VERIFY_IS_TRUE(client.Handshake.MaximumTerminalFrameBytes != 0);
        VERIFY_IS_TRUE(client.Handshake.HeartbeatMilliseconds >= 250);
    }

    void RuntimeProductHostProcessTests::PingAndRuntimeStatusReportConcreteHost()
    {
        ProductRuntimeTestIdentity identity;
        TaskRuntimeProcess process{ identity };
        _verifyReady(process);
        constexpr std::string_view clientId{ "71000000-0000-4000-8000-000000000002" };
        auto client = RequireProductControlClient(identity, clientId);

        _verifyPing(
            client.Channel,
            "72000000-0000-4000-8000-000000000001",
            client.Handshake.RuntimeInstanceId,
            clientId);

        const auto response = _requireSucceededResponse(
            client.Channel,
            "72000000-0000-4000-8000-000000000002",
            ControlMethod::RuntimeStatus);
        const auto status = DecodeRuntimeStatusResult(response.ResultJson);
        VERIFY_IS_TRUE(status.Succeeded());
        VERIFY_ARE_EQUAL(identity.Identity().ProtocolIdentity, status.Value.RuntimeIdentity);
        VERIFY_ARE_EQUAL(client.Handshake.RuntimeInstanceId, status.Value.RuntimeInstanceId);
        VERIFY_ARE_EQUAL(static_cast<uint32_t>(process.ProcessId()), status.Value.ProcessId);
        VERIFY_ARE_EQUAL(std::string{ RuntimeFoundationReadyState }, status.Value.State);
    }

    void RuntimeProductHostProcessTests::ReconnectPreservesRuntimeInstanceAndProcess()
    {
        ProductRuntimeTestIdentity identity;
        TaskRuntimeProcess process{ identity };
        _verifyReady(process);
        const auto processId = process.ProcessId();

        auto first = RequireProductControlClient(
            identity,
            "71000000-0000-4000-8000-000000000003");
        const auto runtimeInstanceId = first.Handshake.RuntimeInstanceId;
        first.Channel.Close();

        constexpr std::string_view secondClientId{ "71000000-0000-4000-8000-000000000004" };
        auto second = RequireProductControlClient(identity, secondClientId);
        VERIFY_ARE_EQUAL(runtimeInstanceId, second.Handshake.RuntimeInstanceId);
        VERIFY_ARE_EQUAL(processId, process.ProcessId());
        _verifyPing(
            second.Channel,
            "72000000-0000-4000-8000-000000000003",
            runtimeInstanceId,
            secondClientId);
    }

    void RuntimeProductHostProcessTests::SecondIdentityInstanceExitsThreeWithoutDisruptingOwner()
    {
        ProductRuntimeTestIdentity identity;
        TaskRuntimeProcess owner{ identity };
        _verifyReady(owner);
        constexpr std::string_view ownerClientId{ "71000000-0000-4000-8000-000000000005" };
        auto ownerClient = RequireProductControlClient(identity, ownerClientId);

        TaskRuntimeProcess rejected{ identity };
        DWORD rejectedExitCode{};
        VERIFY_IS_TRUE(rejected.WaitForExit(ProductProcessExitTimeoutMilliseconds, rejectedExitCode));
        VERIFY_ARE_EQUAL(DWORD{ 3 }, rejectedExitCode);
        VERIFY_IS_FALSE(rejected.ForcedCleanup());
        VERIFY_IS_TRUE(owner.Running());
        _verifyPing(
            ownerClient.Channel,
            "72000000-0000-4000-8000-000000000004",
            ownerClient.Handshake.RuntimeInstanceId,
            ownerClientId);
    }

    void RuntimeProductHostProcessTests::MalformedPeerIsIsolatedFromHealthyPeers()
    {
        ProductRuntimeTestIdentity identity;
        TaskRuntimeProcess process{ identity };
        _verifyReady(process);
        constexpr std::string_view healthyClientId{ "71000000-0000-4000-8000-000000000006" };
        auto healthy = RequireProductControlClient(identity, healthyClientId);

        auto malformed = ConnectRuntimeControlChannel(
            identity.Identity(),
            ProductControlTimeoutMilliseconds);
        VERIFY_IS_TRUE(malformed.Succeeded());
        VERIFY_IS_TRUE(
            malformed.Channel.Send("{", ProductControlTimeoutMilliseconds).Succeeded());
        const auto malformedClosed = malformed.Channel.Receive(ProductControlTimeoutMilliseconds);
        VERIFY_IS_TRUE(malformedClosed.Status == RuntimeTransportStatus::Disconnected);

        _verifyPing(
            healthy.Channel,
            "72000000-0000-4000-8000-000000000005",
            healthy.Handshake.RuntimeInstanceId,
            healthyClientId);

        constexpr std::string_view newClientId{ "71000000-0000-4000-8000-000000000007" };
        auto newClient = RequireProductControlClient(identity, newClientId);
        VERIFY_ARE_EQUAL(healthy.Handshake.RuntimeInstanceId, newClient.Handshake.RuntimeInstanceId);
        _verifyPing(
            newClient.Channel,
            "72000000-0000-4000-8000-000000000006",
            newClient.Handshake.RuntimeInstanceId,
            newClientId);
    }

    void RuntimeProductHostProcessTests::ShutdownEventStopsGracefullyAndReleasesIdentityResources()
    {
        ProductRuntimeTestIdentity identity;
        TaskRuntimeProcess process{ identity };
        _verifyReady(process);
        auto client = RequireProductControlClient(
            identity,
            "71000000-0000-4000-8000-000000000008");
        const auto originalRuntimeInstanceId = client.Handshake.RuntimeInstanceId;
        auto stalledHello = ConnectRuntimeControlChannel(
            identity.Identity(),
            ProductControlTimeoutMilliseconds);
        VERIFY_IS_TRUE(stalledHello.Succeeded());

        VERIFY_IS_TRUE(process.SignalShutdown());
        DWORD exitCode{};
        VERIFY_IS_TRUE(process.WaitForExit(ProductProcessExitTimeoutMilliseconds, exitCode));
        VERIFY_ARE_EQUAL(DWORD{ 0 }, exitCode);
        VERIFY_IS_FALSE(process.Running());
        VERIFY_IS_FALSE(process.ForcedCleanup());

        const auto disconnected = client.Channel.Receive(ProductControlTimeoutMilliseconds);
        VERIFY_IS_FALSE(disconnected.Succeeded());
        const auto stalledDisconnected = stalledHello.Channel.Receive(ProductControlTimeoutMilliseconds);
        VERIFY_IS_FALSE(stalledDisconnected.Succeeded());
        client.Channel.Close();
        stalledHello.Channel.Close();

        {
            const auto reacquired = BootstrapCurrentUserRuntime(
                identity.Channel(),
                identity.SettingsRoot());
            VERIFY_IS_TRUE(reacquired.Ready());
            VERIFY_IS_TRUE(static_cast<bool>(reacquired.Instance));
            VERIFY_IS_TRUE(static_cast<bool>(reacquired.ControlPipe));
        }

        TaskRuntimeProcess restarted{ identity };
        _verifyReady(restarted);
        auto restartedClient = RequireProductControlClient(
            identity,
            "71000000-0000-4000-8000-000000000009");
        VERIFY_ARE_NOT_EQUAL(
            originalRuntimeInstanceId,
            restartedClient.Handshake.RuntimeInstanceId);
        VERIFY_IS_TRUE(restarted.SignalShutdown());
        DWORD restartedExitCode{};
        VERIFY_IS_TRUE(restarted.WaitForExit(
            ProductProcessExitTimeoutMilliseconds,
            restartedExitCode));
        VERIFY_ARE_EQUAL(DWORD{ 0 }, restartedExitCode);
        VERIFY_IS_FALSE(restarted.ForcedCleanup());
    }

    void RuntimeProductHostProcessTests::RealRuntimePersistsTerminalUiCreateAndQuery()
    {
        ProductRuntimeTestIdentity identity;
        TaskRuntimeProcess process{ identity };
        _verifyReady(process);
        auto client = RequireProductControlClient(
            identity,
            "71000000-0000-4000-8000-000000000010");

        TaskUiCommandRequest create;
        create.CommandId = "73000000-0000-4000-8000-000000000001";
        create.TaskId = "74000000-0000-4000-8000-000000000001";
        create.Operation = TaskUiCommandOperation::Create;
        create.Draft = TaskUiCreateDraft{};
        create.Draft->WorkspaceId = "75000000-0000-4000-8000-000000000001";
        create.Draft->Title = "Real Runtime Task";
        const auto source = identity.SettingsRoot().u8string();
        create.Draft->SourceRoot.assign(
            reinterpret_cast<const char*>(source.data()),
            source.size());
        create.Draft->ExecutionMode = TaskProjectionExecutionMode::Direct;
        const auto encodedCreate = EncodeTaskUiCommandRequest(create);
        VERIFY_IS_TRUE(encodedCreate.Succeeded);
        ControlRequest request;
        request.RequestId = "72000000-0000-4000-8000-000000000011";
        request.Method = ControlMethod::SubmitCommand;
        request.PayloadJson = encodedCreate.Payload;
        VERIFY_IS_TRUE(client.Channel.Send(EncodeControlRequest(request).Payload, ProductControlTimeoutMilliseconds).Succeeded());
        const auto received = client.Channel.Receive(ProductControlTimeoutMilliseconds);
        VERIFY_IS_TRUE(received.Succeeded());
        const auto createdEnvelope = DecodeControlResponse(received.Payload);
        VERIFY_IS_TRUE(createdEnvelope.Succeeded());
        VERIFY_IS_TRUE(createdEnvelope.Value.Succeeded);
        const auto created = DecodeTaskUiCommandResult(createdEnvelope.Value.ResultJson);
        VERIFY_IS_TRUE(created.Succeeded());
        VERIFY_ARE_EQUAL(create.TaskId, created.Value.TaskId);

        TaskProjectionQueryRequest query;
        query.TaskId = create.TaskId;
        query.IncludeCompleted = true;
        query.EventLimit = 16;
        request.RequestId = "72000000-0000-4000-8000-000000000012";
        request.Method = ControlMethod::QueryTask;
        request.PayloadJson = EncodeTaskProjectionQueryRequest(query).Payload;
        VERIFY_IS_TRUE(client.Channel.Send(EncodeControlRequest(request).Payload, ProductControlTimeoutMilliseconds).Succeeded());
        const auto queryWire = client.Channel.Receive(ProductControlTimeoutMilliseconds);
        VERIFY_IS_TRUE(queryWire.Succeeded());
        const auto queryEnvelope = DecodeControlResponse(queryWire.Payload);
        VERIFY_IS_TRUE(queryEnvelope.Succeeded());
        VERIFY_IS_TRUE(queryEnvelope.Value.Succeeded);
        const auto projected = DecodeTaskProjectionQueryResult(queryEnvelope.Value.ResultJson);
        VERIFY_IS_TRUE(projected.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, projected.Value.Tasks.size());
        VERIFY_ARE_EQUAL(std::string{ "Real Runtime Task" }, projected.Value.Tasks.front().Title);
    }
}
