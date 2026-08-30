// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeClientTestSupport.h"

#include <atomic>
#include <thread>
#include <unordered_set>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeClientLocalTests::TestSupport;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        RuntimeClientSubscriptionEvent PullKind(
            const RuntimeClientSubscriptionHandle& subscription,
            const RuntimeClientSubscriptionEventKind kind)
        {
            const auto value = PullSubscriptionEvent(subscription.Stream);
            VERIFY_IS_TRUE(value.has_value());
            VERIFY_IS_TRUE(value && value->Kind == kind);
            return value.value_or(RuntimeClientSubscriptionEvent{});
        }

        RuntimeClientSubscribeOptions Options(const std::string_view params = "{}")
        {
            RuntimeClientSubscribeOptions options;
            options.ParamsJson = params;
            return options;
        }

        bool HasMethod(const std::vector<ControlRequest>& requests, const ControlMethod method)
        {
            return std::ranges::find_if(requests, [method](const auto& request) {
                return request.Method == method;
            }) != requests.end();
        }
    }

    class TaskRuntimeClientSubscriptionIntegrationTests
    {
        TEST_CLASS(TaskRuntimeClientSubscriptionIntegrationTests);

        TEST_METHOD(SubscribeWhileConnectingSnapshotsWhenReady);
        TEST_METHOD(SnapshotResponseMayCompleteBeforeSendReturns);
        TEST_METHOD(SnapshotPrecedesBufferedNotification);
        TEST_METHOD(DuplicateAndUnknownAdvanceWithoutVisibleDelta);
        TEST_METHOD(CursorFailuresReconnectAndUseFreshWireIdentity);
        TEST_METHOD(TwoSubscriptionsCorrelateOutOfOrderResponses);
        TEST_METHOD(UnsubscribeIsSafeInDesiredInflightAndLivePhases);
        TEST_METHOD(FullStreamReconnectsWithoutSilentCursorLoss);
        TEST_METHOD(StopAndFacadeDestructionWakeWithClientStopped);
        TEST_METHOD(SnapshotDisconnectCeilingFaultsStream);
        TEST_METHOD(UserSnapshotAndUnsubscribeIdsAreGloballyUnique);
    };

    void TaskRuntimeClientSubscriptionIntegrationTests::SubscribeWhileConnectingSnapshotsWhenReady()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 2);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        InstallSubscriptionResponder(connection, CanonicalSubscriptionIds[0], 1, 7, "{\"ready\":true}");
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse(), true);
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        const auto subscription = client.Subscribe(Options("{\"scope\":\"all\"}"));
        VERIFY_IS_TRUE(subscription.Accepted);
        fixture.Connector->ReleaseConnect();
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 1));
        const auto snapshot = PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, snapshot.Cursor);
        VERIFY_ARE_EQUAL(std::string{ "{\"ready\":true}" }, snapshot.SnapshotJson);
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::SnapshotResponseMayCompleteBeforeSendReturns()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 2);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        connection->BlockNextSendAfterResponse();
        connection->SetNextAutoResponseHandler([](const ControlRequest& request) {
            return SnapshotResponse(request, CanonicalSubscriptionIds[0], 1, 3);
        });
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        const auto subscription = client.Subscribe();
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connection->ResponseQueuedEvent(), 2000));
        VERIFY_IS_TRUE(PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot).Cursor == 3);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_TIMEOUT }, WaitForSingleObject(connection->SendExitedEvent(), 0));
        connection->ReleaseSend();
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connection->SendExitedEvent(), 2000));
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::SnapshotPrecedesBufferedNotification()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 2);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        InstallSubscriptionResponder(connection, CanonicalSubscriptionIds[0], 2, 10, "{\"base\":10}");
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        std::atomic_bool queued{};
        std::thread notification{ [&] {
            if (WaitForSingleObject(connection->ResponseQueuedEvent(), 2000) == WAIT_OBJECT_0)
            {
                queued.store(connection->QueueInboundNotification(SubscriptionNotification(
                    CanonicalSubscriptionIds[0], 2, 11, ControlNotificationKind::ProjectionChanged,
                    "{\"delta\":11}")), std::memory_order_release);
            }
        } };
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        const auto subscription = client.Subscribe();
        const auto snapshot = PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
        const auto delta = PullKind(subscription, RuntimeClientSubscriptionEventKind::Notification);
        notification.join();
        VERIFY_IS_TRUE(queued.load(std::memory_order_acquire));
        VERIFY_ARE_EQUAL(uint64_t{ 10 }, snapshot.Cursor);
        VERIFY_ARE_EQUAL(uint64_t{ 11 }, delta.Cursor);
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::DuplicateAndUnknownAdvanceWithoutVisibleDelta()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 2);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        InstallSubscriptionResponder(connection, CanonicalSubscriptionIds[0], 1, 10);
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        const auto subscription = client.Subscribe();
        (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_IS_TRUE(connection->QueueInboundNotification(SubscriptionNotification(
            CanonicalSubscriptionIds[0], 1, 10)));
        QueueUnknownSubscriptionNotification(
            connection, CanonicalSubscriptionIds[0], 1, 11, "future_optional_event");
        VERIFY_IS_TRUE(connection->QueueInboundNotification(SubscriptionNotification(
            CanonicalSubscriptionIds[0], 1, 12, ControlNotificationKind::ProjectionChanged,
            "{\"delta\":12}")));
        const auto delta = PullKind(subscription, RuntimeClientSubscriptionEventKind::Notification);
        VERIFY_ARE_EQUAL(uint64_t{ 12 }, delta.Cursor);
        VERIFY_IS_TRUE(subscription.Stream->Pull(0).Status == RuntimeClientSubscriptionPullStatus::Timeout);
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::CursorFailuresReconnectAndUseFreshWireIdentity()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 6);
        std::array<std::shared_ptr<FakeRuntimeClientConnectionState>, 4> connections;
        for (size_t index = 0; index < connections.size(); ++index)
        {
            connections[index] = std::make_shared<FakeRuntimeClientConnectionState>();
            InstallSubscriptionResponder(
                connections[index], CanonicalSubscriptionIds[index], index + 1, (index + 1) * 10);
            fixture.Connector->EnqueueSuccess(connections[index], ValidHelloResponse());
        }
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connections[0]));
        const auto subscription = client.Subscribe();
        const auto first = PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_IS_TRUE(connections[0]->QueueInboundNotification(SubscriptionNotification(
            CanonicalSubscriptionIds[0], 1, 12)));
        (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::ResnapshotRequired);
        const auto second = PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_IS_TRUE(connections[1]->QueueInboundNotification(SubscriptionNotification(
            CanonicalSubscriptionIds[1], 99, 21)));
        (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::ResnapshotRequired);
        const auto third = PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_IS_TRUE(connections[2]->QueueInboundNotification(SubscriptionNotification(
            CanonicalSubscriptionIds[2], 3, 31, ControlNotificationKind::SubscriptionClosed)));
        (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::ResnapshotRequired);
        const auto fourth = PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_ARE_EQUAL(uint64_t{ 10 }, first.Cursor);
        VERIFY_ARE_EQUAL(uint64_t{ 20 }, second.Cursor);
        VERIFY_ARE_EQUAL(uint64_t{ 30 }, third.Cursor);
        VERIFY_ARE_EQUAL(uint64_t{ 40 }, fourth.Cursor);
        for (const auto& connection : connections)
        {
            VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 1));
        }
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::TwoSubscriptionsCorrelateOutOfOrderResponses()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 4);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        const auto first = client.Subscribe(Options("{\"slot\":1}"));
        const auto second = client.Subscribe(Options("{\"slot\":2}"));
        VERIFY_IS_TRUE(first.Accepted && second.Accepted);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 2));
        const auto requests = connection->OutboundRequests();
        const auto one = std::ranges::find_if(requests, [](const auto& request) {
            return request.PayloadJson.find("\"slot\":1") != std::string::npos;
        });
        const auto two = std::ranges::find_if(requests, [](const auto& request) {
            return request.PayloadJson.find("\"slot\":2") != std::string::npos;
        });
        VERIFY_IS_TRUE(one != requests.end() && two != requests.end());
        if (one == requests.end() || two == requests.end())
        {
            (void)StopAndWait(client);
            return;
        }
        VERIFY_IS_TRUE(connection->QueueInboundResponse(SnapshotResponse(
            *two, CanonicalSubscriptionIds[1], 1, 22, "{\"slot\":2}")));
        VERIFY_IS_TRUE(connection->QueueInboundResponse(SnapshotResponse(
            *one, CanonicalSubscriptionIds[0], 1, 11, "{\"slot\":1}")));
        VERIFY_ARE_EQUAL(uint64_t{ 22 }, PullKind(second, RuntimeClientSubscriptionEventKind::Snapshot).Cursor);
        VERIFY_ARE_EQUAL(uint64_t{ 11 }, PullKind(first, RuntimeClientSubscriptionEventKind::Snapshot).Cursor);
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::UnsubscribeIsSafeInDesiredInflightAndLivePhases()
    {
        {
            RuntimeClientDependenciesFixture fixture;
            fixture.EnqueueRequestIds(0, 2);
            auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
            fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse(), true);
            TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
            VERIFY_IS_TRUE(client.Start());
            VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
            const auto subscription = client.Subscribe();
            VERIFY_IS_TRUE(subscription.Stream->RequestUnsubscribe());
            VERIFY_IS_TRUE(PullKind(subscription, RuntimeClientSubscriptionEventKind::Removed).Kind ==
                           RuntimeClientSubscriptionEventKind::Removed);
            fixture.Connector->ReleaseConnect();
            VERIFY_ARE_EQUAL(DWORD{ WAIT_TIMEOUT }, WaitForSingleObject(connection->SendEnteredEvent(), 50));
            VERIFY_IS_TRUE(StopAndWait(client));
        }
        {
            RuntimeClientDependenciesFixture fixture;
            fixture.EnqueueRequestIds(0, 3);
            auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
            fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
            TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
            VERIFY_IS_TRUE(client.Start());
            VERIFY_IS_TRUE(WaitReady(client, *connection));
            const auto subscription = client.Subscribe();
            VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 1));
            VERIFY_IS_TRUE(subscription.Stream->RequestUnsubscribe());
            const auto snapshotRequest = connection->OutboundRequests().front();
            VERIFY_IS_TRUE(connection->QueueInboundResponse(SnapshotResponse(
                snapshotRequest, CanonicalSubscriptionIds[0], 1, 1)));
            (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
            VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 2));
            const auto unsubscribe = connection->OutboundRequests().back();
            VERIFY_IS_TRUE(unsubscribe.Method == ControlMethod::Unsubscribe);
            VERIFY_IS_TRUE(connection->QueueInboundResponse(EmptySuccessResponse(unsubscribe)));
            (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::Removed);
            VERIFY_IS_TRUE(StopAndWait(client));
        }
        {
            RuntimeClientDependenciesFixture fixture;
            fixture.EnqueueRequestIds(0, 3);
            auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
            InstallSubscriptionResponder(connection, CanonicalSubscriptionIds[0], 1, 1);
            fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
            TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
            VERIFY_IS_TRUE(client.Start());
            VERIFY_IS_TRUE(WaitReady(client, *connection));
            const auto subscription = client.Subscribe();
            (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
            VERIFY_IS_TRUE(subscription.Stream->RequestUnsubscribe());
            (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::Removed);
            VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 2));
            VERIFY_IS_TRUE(connection->OutboundRequests().back().Method == ControlMethod::Unsubscribe);
            VERIFY_IS_TRUE(StopAndWait(client));
        }
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::FullStreamReconnectsWithoutSilentCursorLoss()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 4);
        auto first = std::make_shared<FakeRuntimeClientConnectionState>();
        auto second = std::make_shared<FakeRuntimeClientConnectionState>();
        InstallSubscriptionResponder(first, CanonicalSubscriptionIds[0], 1, 10);
        const auto handlerEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const auto releaseHandler = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY_IS_NOT_NULL(handlerEntered);
        VERIFY_IS_NOT_NULL(releaseHandler);
        second->SetAutoResponseHandler([&](const ControlRequest& request) {
            SetEvent(handlerEntered);
            WaitForSingleObject(releaseHandler, 2000);
            return SnapshotResponse(request, CanonicalSubscriptionIds[1], 2, 11, "{\"cursor\":11}");
        });
        fixture.Connector->EnqueueSuccess(first, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(second, ValidHelloResponse());
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *first));
        auto options = Options();
        options.MaximumQueuedEvents = 1;
        const auto subscription = client.Subscribe(options);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*first, 1));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(first->ResponseQueuedEvent(), 2000));
        VERIFY_IS_TRUE(first->QueueInboundNotification(SubscriptionNotification(
            CanonicalSubscriptionIds[0], 1, 11)));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(handlerEntered, 2000));
        VERIFY_ARE_EQUAL(uint64_t{ 10 }, PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot).Cursor);
        SetEvent(releaseHandler);
        VERIFY_ARE_EQUAL(uint64_t{ 11 }, PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot).Cursor);
        VERIFY_IS_TRUE(StopAndWait(client));
        CloseHandle(handlerEntered);
        CloseHandle(releaseHandler);
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::StopAndFacadeDestructionWakeWithClientStopped()
    {
        {
            RuntimeClientDependenciesFixture fixture;
            auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
            fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse(), true);
            TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
            VERIFY_IS_TRUE(client.Start());
            VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
            const auto subscription = client.Subscribe();
            client.RequestStop();
            VERIFY_IS_TRUE(client.WaitForStopped());
            (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::ClientStopped);
        }
        {
            RuntimeClientDependenciesFixture fixture;
            fixture.EnqueueRequestIds(0, 2);
            auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
            InstallSubscriptionResponder(connection, CanonicalSubscriptionIds[0], 1, 1);
            fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
            auto client = std::make_unique<TaskRuntimeClient>(ValidConfiguration(), fixture.Dependencies());
            VERIFY_IS_TRUE(client->Start());
            VERIFY_IS_TRUE(WaitReady(*client, *connection));
            const auto subscription = client->Subscribe();
            (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
            client.reset();
            (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::ClientStopped);
        }
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::SnapshotDisconnectCeilingFaultsStream()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 4);
        auto configuration = ValidConfiguration();
        configuration.MaximumSubscriptionAttempts = 2;
        auto first = std::make_shared<FakeRuntimeClientConnectionState>();
        auto second = std::make_shared<FakeRuntimeClientConnectionState>();
        auto third = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(first, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(second, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(third, ValidHelloResponse(), true);
        TaskRuntimeClient client{ configuration, fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *first));
        const auto subscription = client.Subscribe();
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*first, 1));
        first->QueueInboundFailure(RuntimeTransportStatus::Disconnected, "first disconnect", ERROR_BROKEN_PIPE);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*second, 1));
        second->QueueInboundFailure(RuntimeTransportStatus::Disconnected, "second disconnect", ERROR_BROKEN_PIPE);
        const auto fault = PullKind(subscription, RuntimeClientSubscriptionEventKind::Faulted);
        VERIFY_IS_TRUE(fault.TransportStatus == RuntimeTransportStatus::Disconnected);
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSubscriptionIntegrationTests::UserSnapshotAndUnsubscribeIdsAreGloballyUnique()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 4);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        InstallSubscriptionResponder(connection, CanonicalSubscriptionIds[0], 1, 1);
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        const auto subscription = client.Subscribe();
        const auto user = client.Submit(ControlMethod::Ping);
        (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_IS_TRUE(subscription.Stream->RequestUnsubscribe());
        (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::Removed);
        VERIFY_IS_TRUE(WaitReady(user));
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 3));
        const auto requests = connection->OutboundRequests();
        std::unordered_set<std::string> identifiers;
        for (const auto& request : requests)
        {
            identifiers.emplace(request.RequestId);
        }
        VERIFY_ARE_EQUAL(size_t{ 3 }, requests.size());
        VERIFY_ARE_EQUAL(size_t{ 3 }, identifiers.size());
        VERIFY_IS_TRUE(HasMethod(requests, ControlMethod::Ping));
        VERIFY_IS_TRUE(HasMethod(requests, ControlMethod::SnapshotAndSubscribe));
        VERIFY_IS_TRUE(HasMethod(requests, ControlMethod::Unsubscribe));
        VERIFY_IS_TRUE(StopAndWait(client));
    }
}
