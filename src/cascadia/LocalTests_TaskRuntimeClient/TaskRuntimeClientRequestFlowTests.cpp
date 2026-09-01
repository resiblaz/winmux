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
    namespace
    {
        ControlResponse SuccessfulResponse(const std::string& requestId)
        {
            ControlResponse response;
            response.RequestId = requestId;
            response.Succeeded = true;
            response.ResultJson = "{\"ok\":true}";
            return response;
        }
    }

    class TaskRuntimeClientRequestFlowTests
    {
        TEST_CLASS(TaskRuntimeClientRequestFlowTests);

        TEST_METHOD(ConnectRequestResponseAndLifecycle);
        TEST_METHOD(ResponseCanCompleteBeforeSendReturns);
        TEST_METHOD(ResponsesMayArriveOutOfOrder);
        TEST_METHOD(DuplicateResponseIsIgnoredAndConnectionRemainsUsable);
        TEST_METHOD(UnknownCurrentEpochResponseFaultsClient);
        TEST_METHOD(OneEpochUsesOnlyOneWriter);
        TEST_METHOD(SubscriptionMethodsRemainOutsideRequestCore);
    };

    void TaskRuntimeClientRequestFlowTests::ConnectRequestResponseAndLifecycle()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        connection->EnableAutoResponse();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Valid());
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));

        const auto request = client.Submit(ControlMethod::Ping, "{\"probe\":1}");
        VERIFY_IS_TRUE(request.Accepted);
        VERIFY_IS_TRUE(WaitReady(request));
        const auto result = request.Completion.get();
        VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::Succeeded);
        VERIFY_ARE_EQUAL(request.OperationId, result.OperationId);
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, result.Attempts);
        VERIFY_ARE_EQUAL(std::string{ CanonicalRequestIds[0] }, result.LastRequestId);
        VERIFY_IS_TRUE(result.Response.has_value());

        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientRequestFlowTests::ResponseCanCompleteBeforeSendReturns()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        connection->EnableAutoResponse();
        connection->BlockNextSendAfterResponse();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));

        const auto request = client.Submit(ControlMethod::QueryTask, "{\"taskId\":\"one\"}");
        VERIFY_IS_TRUE(request.Accepted);
        const auto responseQueued = WaitForSingleObject(connection->ResponseQueuedEvent(), 2000);
        const auto completedBeforeSendReturned = WaitReady(request, 1000);
        connection->ReleaseSend();

        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, responseQueued);
        VERIFY_IS_TRUE(completedBeforeSendReturned);
        VERIFY_IS_TRUE(request.Completion.get().Outcome == RuntimeClientRequestOutcome::Succeeded);
        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientRequestFlowTests::ResponsesMayArriveOutOfOrder()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 2);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));

        const auto first = client.Submit(ControlMethod::Ping);
        const auto second = client.Submit(ControlMethod::QueryTask, "{\"taskId\":\"two\"}");
        VERIFY_IS_TRUE(first.Accepted);
        VERIFY_IS_TRUE(second.Accepted);
        VERIFY_IS_TRUE(WaitUntil([&] { return connection->OutboundRequests().size() == 2; }));

        const auto outbound = connection->OutboundRequests();
        VERIFY_ARE_EQUAL(size_t{ 2 }, outbound.size());
        VERIFY_IS_TRUE(connection->QueueInboundResponse(SuccessfulResponse(outbound[1].RequestId)));
        VERIFY_IS_TRUE(connection->QueueInboundResponse(SuccessfulResponse(outbound[0].RequestId)));
        VERIFY_IS_TRUE(WaitReady(first));
        VERIFY_IS_TRUE(WaitReady(second));
        VERIFY_ARE_EQUAL(outbound[0].RequestId, first.Completion.get().LastRequestId);
        VERIFY_ARE_EQUAL(outbound[1].RequestId, second.Completion.get().LastRequestId);

        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientRequestFlowTests::DuplicateResponseIsIgnoredAndConnectionRemainsUsable()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 2);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));

        const auto first = client.Submit(ControlMethod::Ping);
        VERIFY_IS_TRUE(WaitUntil([&] { return connection->OutboundRequests().size() == 1; }));
        const auto firstId = connection->OutboundRequests()[0].RequestId;
        const auto firstResponse = SuccessfulResponse(firstId);
        VERIFY_IS_TRUE(connection->QueueInboundResponse(firstResponse));
        VERIFY_IS_TRUE(connection->QueueInboundResponse(firstResponse));
        VERIFY_IS_TRUE(WaitReady(first));
        VERIFY_IS_TRUE(first.Completion.get().Outcome == RuntimeClientRequestOutcome::Succeeded);

        const auto second = client.Submit(ControlMethod::RuntimeStatus);
        VERIFY_IS_TRUE(WaitUntil([&] { return connection->OutboundRequests().size() == 2; }));
        const auto secondId = connection->OutboundRequests()[1].RequestId;
        VERIFY_IS_TRUE(connection->QueueInboundResponse(SuccessfulResponse(secondId)));
        VERIFY_IS_TRUE(WaitReady(second));
        VERIFY_IS_TRUE(second.Completion.get().Outcome == RuntimeClientRequestOutcome::Succeeded);
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Ready);
        VERIFY_ARE_EQUAL(CurrentProtocolMinor, client.ProtocolMinor());
        VERIFY_ARE_EQUAL(AllProtocolCapabilities, client.NegotiatedCapabilities());

        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientRequestFlowTests::UnknownCurrentEpochResponseFaultsClient()
    {
        RuntimeClientDependenciesFixture fixture;
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));
        VERIFY_IS_TRUE(connection->QueueInboundResponse(
            SuccessfulResponse(std::string{ CanonicalRequestIds[5] })));
        VERIFY_IS_TRUE(WaitUntil([&] { return client.State() == TaskRuntimeClientState::Faulted; }));
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());

        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientRequestFlowTests::OneEpochUsesOnlyOneWriter()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 3);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        connection->EnableAutoResponse();
        connection->BlockNextSendAfterResponse();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));
        const auto first = client.Submit(ControlMethod::Ping);
        const auto second = client.Submit(ControlMethod::QueryTask);
        const auto third = client.Submit(ControlMethod::RuntimeStatus);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connection->ResponseQueuedEvent(), 2000));
        Sleep(25);
        const auto blockedOutboundCount = connection->OutboundRequests().size();
        connection->ReleaseSend();

        VERIFY_ARE_EQUAL(size_t{ 1 }, blockedOutboundCount);
        VERIFY_IS_TRUE(WaitReady(first));
        VERIFY_IS_TRUE(WaitReady(second));
        VERIFY_IS_TRUE(WaitReady(third));
        VERIFY_ARE_EQUAL(size_t{ 1 }, connection->MaximumConcurrentSends());
        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientRequestFlowTests::SubscriptionMethodsRemainOutsideRequestCore()
    {
        RuntimeClientDependenciesFixture fixture;
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        const auto snapshot = client.Submit(ControlMethod::SnapshotAndSubscribe);
        const auto unsubscribe = client.Submit(ControlMethod::Unsubscribe);
        VERIFY_IS_FALSE(snapshot.Accepted);
        VERIFY_IS_FALSE(unsubscribe.Accepted);
        VERIFY_IS_TRUE(WaitReady(snapshot));
        VERIFY_IS_TRUE(WaitReady(unsubscribe));
        VERIFY_IS_TRUE(snapshot.Completion.get().Outcome == RuntimeClientRequestOutcome::InvalidRequest);
        VERIFY_IS_TRUE(unsubscribe.Completion.get().Outcome == RuntimeClientRequestOutcome::InvalidRequest);
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.RequestIds->Calls());
    }
}
