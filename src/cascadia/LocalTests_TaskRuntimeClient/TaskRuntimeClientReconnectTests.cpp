// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeClientTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeClientLocalTests::TestSupport;

namespace TaskRuntimeClientLocalTests
{
    class TaskRuntimeClientReconnectTests
    {
        TEST_CLASS(TaskRuntimeClientReconnectTests);

        TEST_METHOD(ReadOnlyRequestRetriesWithFreshRequestId);
        TEST_METHOD(TransmittedMutationReturnsOutcomeUnknownWithoutReplay);
        TEST_METHOD(NotSentRequestSurvivesEpochRetirement);
        TEST_METHOD(TransientConnectFailureBacksOffThenConnects);
        TEST_METHOD(ProtocolAndPermanentFailuresFaultWithoutRetry);
        TEST_METHOD(FatalConnectFailureCompletesQueuedRequest);
        TEST_METHOD(ImmediateDisconnectsAccumulateBackoffUntilHealthyResponse);
        TEST_METHOD(ReusedRequestIdFailsClosedBeforeWire);
    };

    void TaskRuntimeClientReconnectTests::ReadOnlyRequestRetriesWithFreshRequestId()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 2);
        auto firstConnection = std::make_shared<FakeRuntimeClientConnectionState>();
        firstConnection->QueueSendFailure(
            RuntimeTransportStatus::Disconnected,
            "first epoch disconnected",
            ERROR_BROKEN_PIPE);
        auto secondConnection = std::make_shared<FakeRuntimeClientConnectionState>();
        secondConnection->EnableAutoResponse();
        fixture.Connector->EnqueueSuccess(firstConnection, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(
            secondConnection,
            ValidHelloResponse("20000000-0000-4000-8000-000000000302"));

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));
        const auto request = client.Submit(ControlMethod::QueryTask, "{\"taskId\":\"retry\"}");
        VERIFY_IS_TRUE(request.Accepted);
        VERIFY_IS_TRUE(WaitReady(request));

        const auto result = request.Completion.get();
        VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::Succeeded);
        VERIFY_ARE_EQUAL(uint32_t{ 2 }, result.Attempts);
        VERIFY_ARE_EQUAL(std::string{ CanonicalRequestIds[1] }, result.LastRequestId);
        VERIFY_ARE_EQUAL(std::string{ CanonicalRequestIds[0] }, firstConnection->OutboundRequests()[0].RequestId);
        VERIFY_ARE_EQUAL(std::string{ CanonicalRequestIds[1] }, secondConnection->OutboundRequests()[0].RequestId);
        VERIFY_ARE_EQUAL(size_t{ 2 }, fixture.Connector->Calls().size());

        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*firstConnection));
        VERIFY_IS_TRUE(CancelPrecedesClose(*secondConnection));
    }

    void TaskRuntimeClientReconnectTests::TransmittedMutationReturnsOutcomeUnknownWithoutReplay()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto firstConnection = std::make_shared<FakeRuntimeClientConnectionState>();
        firstConnection->QueueSendFailure(
            RuntimeTransportStatus::Timeout,
            "mutation send timed out",
            WAIT_TIMEOUT);
        auto secondConnection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(firstConnection, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(
            secondConnection,
            ValidHelloResponse("20000000-0000-4000-8000-000000000303"));

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));
        const auto request = client.Submit(ControlMethod::SubmitCommand, "{\"command\":\"run\"}");
        VERIFY_IS_TRUE(request.Accepted);
        VERIFY_IS_TRUE(WaitReady(request));

        const auto result = request.Completion.get();
        VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::OutcomeUnknown);
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, result.Attempts);
        VERIFY_ARE_EQUAL(size_t{ 1 }, firstConnection->OutboundRequests().size());
        VERIFY_IS_TRUE(WaitReady(client));
        VERIFY_IS_TRUE(secondConnection->OutboundRequests().empty());

        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*firstConnection));
        VERIFY_IS_TRUE(CancelPrecedesClose(*secondConnection));
    }

    void TaskRuntimeClientReconnectTests::NotSentRequestSurvivesEpochRetirement()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 2);
        auto firstConnection = std::make_shared<FakeRuntimeClientConnectionState>();
        firstConnection->QueueSendFailure(
            RuntimeTransportStatus::Disconnected,
            "writer disconnected",
            ERROR_BROKEN_PIPE);
        auto secondConnection = std::make_shared<FakeRuntimeClientConnectionState>();
        secondConnection->EnableAutoResponse();
        fixture.Connector->EnqueueSuccess(firstConnection, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(
            secondConnection,
            ValidHelloResponse("20000000-0000-4000-8000-000000000304"));

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));
        const auto mutation = client.Submit(ControlMethod::SubmitCommand);
        const auto queuedRead = client.Submit(ControlMethod::RuntimeStatus);
        VERIFY_IS_TRUE(mutation.Accepted);
        VERIFY_IS_TRUE(queuedRead.Accepted);
        VERIFY_IS_TRUE(WaitReady(mutation));
        VERIFY_IS_TRUE(WaitReady(queuedRead));

        VERIFY_IS_TRUE(mutation.Completion.get().Outcome == RuntimeClientRequestOutcome::OutcomeUnknown);
        const auto readResult = queuedRead.Completion.get();
        VERIFY_IS_TRUE(readResult.Outcome == RuntimeClientRequestOutcome::Succeeded);
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, readResult.Attempts);
        VERIFY_ARE_EQUAL(std::string{ CanonicalRequestIds[1] }, readResult.LastRequestId);
        VERIFY_ARE_EQUAL(size_t{ 1 }, secondConnection->OutboundRequests().size());

        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*firstConnection));
        VERIFY_IS_TRUE(CancelPrecedesClose(*secondConnection));
    }

    void TaskRuntimeClientReconnectTests::TransientConnectFailureBacksOffThenConnects()
    {
        RuntimeClientDependenciesFixture fixture;
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueFailure(
            RuntimeTransportStatus::Timeout,
            RuntimeControlConnectPhase::Connecting,
            false,
            "runtime not ready",
            HandshakeError::None,
            WAIT_TIMEOUT);
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));
        const auto failures = fixture.Backoff->ObservedFailures();
        VERIFY_ARE_EQUAL(size_t{ 1 }, failures.size());
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, failures[0]);
        VERIFY_ARE_EQUAL(size_t{ 2 }, fixture.Connector->Calls().size());

        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientReconnectTests::ProtocolAndPermanentFailuresFaultWithoutRetry()
    {
        {
            RuntimeClientDependenciesFixture fixture;
            fixture.Connector->EnqueueFailure(
                RuntimeTransportStatus::ProtocolRejected,
                RuntimeControlConnectPhase::Handshaking,
                false,
                "protocol rejected");
            TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
            VERIFY_IS_TRUE(client.Start());
            VERIFY_IS_TRUE(WaitUntil([&] { return client.State() == TaskRuntimeClientState::Faulted; }));
            VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
            VERIFY_IS_TRUE(fixture.Backoff->ObservedFailures().empty());
            VERIFY_IS_TRUE(StopAndWait(client));
        }
        {
            RuntimeClientDependenciesFixture fixture;
            fixture.Connector->EnqueueFailure(
                RuntimeTransportStatus::SystemError,
                RuntimeControlConnectPhase::Connecting,
                false,
                "access denied",
                HandshakeError::None,
                ERROR_ACCESS_DENIED);
            TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
            VERIFY_IS_TRUE(client.Start());
            VERIFY_IS_TRUE(WaitUntil([&] { return client.State() == TaskRuntimeClientState::Faulted; }));
            VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
            VERIFY_IS_TRUE(fixture.Backoff->ObservedFailures().empty());
            VERIFY_IS_TRUE(StopAndWait(client));
        }
    }

    void TaskRuntimeClientReconnectTests::ImmediateDisconnectsAccumulateBackoffUntilHealthyResponse()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto first = std::make_shared<FakeRuntimeClientConnectionState>();
        auto second = std::make_shared<FakeRuntimeClientConnectionState>();
        auto healthy = std::make_shared<FakeRuntimeClientConnectionState>();
        auto afterHealthy = std::make_shared<FakeRuntimeClientConnectionState>();
        first->QueueInboundFailure(RuntimeTransportStatus::Disconnected, "flap one", ERROR_BROKEN_PIPE);
        second->QueueInboundFailure(RuntimeTransportStatus::Disconnected, "flap two", ERROR_BROKEN_PIPE);
        healthy->EnableAutoResponse();
        fixture.Connector->EnqueueSuccess(first, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(second, ValidHelloResponse("20000000-0000-4000-8000-000000000305"));
        fixture.Connector->EnqueueSuccess(healthy, ValidHelloResponse("20000000-0000-4000-8000-000000000306"));
        fixture.Connector->EnqueueSuccess(afterHealthy, ValidHelloResponse("20000000-0000-4000-8000-000000000307"));

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *healthy));
        const auto probe = client.Submit(ControlMethod::Ping);
        VERIFY_IS_TRUE(WaitReady(probe));
        VERIFY_IS_TRUE(probe.Completion.get().Outcome == RuntimeClientRequestOutcome::Succeeded);
        const auto healthyEpoch = client.ActiveEpoch();
        healthy->QueueInboundFailure(RuntimeTransportStatus::Disconnected, "after healthy", ERROR_BROKEN_PIPE);
        VERIFY_IS_TRUE(WaitUntil([&] {
            return client.State() == TaskRuntimeClientState::Ready && client.ActiveEpoch() > healthyEpoch;
        }));

        const auto failures = fixture.Backoff->ObservedFailures();
        VERIFY_ARE_EQUAL(size_t{ 3 }, failures.size());
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, failures[0]);
        VERIFY_ARE_EQUAL(uint32_t{ 2 }, failures[1]);
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, failures[2]);
        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*afterHealthy));
    }

    void TaskRuntimeClientReconnectTests::FatalConnectFailureCompletesQueuedRequest()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        fixture.Connector->EnqueueFailure(
            RuntimeTransportStatus::ProtocolRejected,
            RuntimeControlConnectPhase::Handshaking,
            true,
            "incompatible runtime");

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        const auto request = client.Submit(ControlMethod::QueryTask);
        VERIFY_IS_TRUE(request.Accepted);
        fixture.Connector->ReleaseConnect();
        VERIFY_IS_TRUE(WaitReady(request));
        const auto result = request.Completion.get();
        VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::ProtocolError);
        VERIFY_IS_TRUE(result.TransportStatus == RuntimeTransportStatus::ProtocolRejected);
        VERIFY_IS_TRUE(WaitUntil([&] { return client.State() == TaskRuntimeClientState::Faulted; }));
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientReconnectTests::ReusedRequestIdFailsClosedBeforeWire()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.RequestIds->Enqueue(std::string{ CanonicalRequestIds[0] });
        fixture.RequestIds->Enqueue(std::string{ CanonicalRequestIds[0] });
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        connection->EnableAutoResponse();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));
        const auto first = client.Submit(ControlMethod::Ping);
        VERIFY_IS_TRUE(WaitReady(first));
        const auto reused = client.Submit(ControlMethod::Ping);
        VERIFY_IS_FALSE(reused.Accepted);
        VERIFY_IS_TRUE(WaitReady(reused));
        VERIFY_IS_TRUE(reused.Completion.get().Outcome == RuntimeClientRequestOutcome::ProtocolError);
        VERIFY_ARE_EQUAL(size_t{ 1 }, connection->OutboundRequests().size());
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Ready);

        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }
}
