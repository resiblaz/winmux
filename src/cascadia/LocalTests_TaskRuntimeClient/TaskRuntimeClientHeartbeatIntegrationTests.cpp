// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRuntimeClientScheduler.h"
#include "TaskRuntimeClientTestSupport.h"

#include "RuntimeClientHeartbeat.h"

#include <algorithm>
#include <chrono>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeClientLocalTests::TestSupport;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        TaskRuntimeClientConfiguration HeartbeatConfiguration()
        {
            auto configuration = ValidConfiguration();
            configuration.ReceiveTimeoutMilliseconds = 60000;
            return configuration;
        }

        HelloResponse HeartbeatHelloResponse(
            const std::string_view runtimeInstanceId = RuntimeInstanceId)
        {
            auto response = ValidHelloResponse(runtimeInstanceId);
            response.HeartbeatMilliseconds = 1000;
            return response;
        }

        TaskRuntimeClientDependencies DependenciesWithScheduler(
            const RuntimeClientDependenciesFixture& fixture,
            const std::shared_ptr<FakeRuntimeClientScheduler>& scheduler)
        {
            auto dependencies = fixture.Dependencies();
            dependencies.Scheduler = scheduler;
            return dependencies;
        }

        std::vector<ControlRequest> HeartbeatRequests(
            const FakeRuntimeClientConnectionState& connection)
        {
            auto requests = connection.OutboundRequests();
            std::erase_if(requests, [](const auto& request) {
                return request.Method != ControlMethod::Ping ||
                       !IsReservedHeartbeatRequestId(request.RequestId);
            });
            return requests;
        }

        bool WaitForHeartbeatCount(
            const FakeRuntimeClientConnectionState& connection,
            const size_t count,
            const DWORD timeoutMilliseconds = 2000)
        {
            const auto end = GetTickCount64() + timeoutMilliseconds;
            while (HeartbeatRequests(connection).size() < count)
            {
                const auto now = GetTickCount64();
                if (now >= end ||
                    WaitForSingleObject(
                        connection.SendEnteredEvent(),
                        static_cast<DWORD>(end - now)) != WAIT_OBJECT_0)
                {
                    return false;
                }
            }
            return true;
        }

        bool QueueResponseAndWaitProcessed(
            const std::shared_ptr<FakeRuntimeClientConnectionState>& connection,
            const ControlResponse& response,
            const DWORD timeoutMilliseconds = 2000)
        {
            const auto before = connection->InboundDequeuedCount();
            if (!connection->QueueInboundResponse(response))
            {
                return false;
            }
            const auto end = GetTickCount64() + timeoutMilliseconds;
            while (connection->InboundDequeuedCount() <= before)
            {
                const auto now = GetTickCount64();
                if (now >= end ||
                    WaitForSingleObject(
                        connection->InboundDequeuedEvent(),
                        static_cast<DWORD>(end - now)) != WAIT_OBJECT_0)
                {
                    return false;
                }
            }
            const auto now = GetTickCount64();
            return now < end &&
                   WaitForSingleObject(
                       connection->ReceiveEnteredEvent(),
                       static_cast<DWORD>(end - now)) == WAIT_OBJECT_0;
        }

        bool QueueNotificationAndWaitProcessed(
            const std::shared_ptr<FakeRuntimeClientConnectionState>& connection,
            const ControlNotification& notification,
            const DWORD timeoutMilliseconds = 2000)
        {
            const auto before = connection->InboundDequeuedCount();
            if (!connection->QueueInboundNotification(notification))
            {
                return false;
            }
            const auto end = GetTickCount64() + timeoutMilliseconds;
            while (connection->InboundDequeuedCount() <= before)
            {
                const auto now = GetTickCount64();
                if (now >= end ||
                    WaitForSingleObject(
                        connection->InboundDequeuedEvent(),
                        static_cast<DWORD>(end - now)) != WAIT_OBJECT_0)
                {
                    return false;
                }
            }
            const auto now = GetTickCount64();
            return now < end &&
                   WaitForSingleObject(
                       connection->ReceiveEnteredEvent(),
                       static_cast<DWORD>(end - now)) == WAIT_OBJECT_0;
        }

        void VerifyDeadline(
            const std::shared_ptr<FakeRuntimeClientScheduler>& scheduler,
            const size_t index,
            const RuntimeClientMonotonicMilliseconds expected)
        {
            const auto deadlines = scheduler->ObservedDeadlines();
            VERIFY_IS_TRUE(index < deadlines.size());
            if (index < deadlines.size())
            {
                VERIFY_ARE_EQUAL(expected, deadlines[index]);
            }
        }

        size_t CompletedWaits(const std::shared_ptr<FakeRuntimeClientScheduler>& scheduler) noexcept
        {
            return scheduler->DeadlineWakeCount() + scheduler->SignalWakeCount() +
                   scheduler->FailedWaitCount();
        }

        bool StopWithBoundedSchedulerCleanup(
            TaskRuntimeClient& client,
            const std::shared_ptr<FakeRuntimeClientScheduler>& scheduler,
            const RuntimeClientMonotonicMilliseconds emergencyDeadline) noexcept
        {
            const auto completedBefore = CompletedWaits(scheduler);
            const auto signalsBefore = scheduler->SignalWakeCount();
            client.RequestStop();
            auto completed = scheduler->WaitForCompletedWaits(completedBefore + 1);
            if (!completed)
            {
                scheduler->ReleaseDeadline();
                (void)scheduler->AdvanceTo((std::max)(scheduler->Now(), emergencyDeadline));
                scheduler->Pulse();
                completed = scheduler->WaitForCompletedWaits(completedBefore + 1);
            }
            const auto stopped = client.WaitForStopped();
            return completed && scheduler->SignalWakeCount() > signalsBefore && stopped &&
                   client.State() == TaskRuntimeClientState::Stopped;
        }
    }

    class TaskRuntimeClientHeartbeatIntegrationTests
    {
        TEST_CLASS(TaskRuntimeClientHeartbeatIntegrationTests);

        TEST_METHOD(HalfIntervalPingAndAckRearmExactDeadlines);
        TEST_METHOD(MissingAckRetiresAtFullDeadlineThenBacksOff);
        TEST_METHOD(AdvancePastFullDeadlineDoesNotSendLatePing);
        TEST_METHOD(PublicResponseRearmsHeartbeatWindow);
        TEST_METHOD(SupersededOldAckDoesNotRefreshLiveness);
        TEST_METHOD(SubscriptionResponseRearmsHeartbeatWindow);
        TEST_METHOD(SubscriptionNotificationRearmsHeartbeatWindow);
    };

    void TaskRuntimeClientHeartbeatIntegrationTests::HalfIntervalPingAndAckRearmExactDeadlines()
    {
        RuntimeClientDependenciesFixture fixture;
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, HeartbeatHelloResponse());
        TaskRuntimeClient client{
            HeartbeatConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        VerifyDeadline(scheduler, 0, 500);

        VERIFY_IS_TRUE(scheduler->AdvanceTo(499));
        VERIFY_IS_TRUE(HeartbeatRequests(*connection).empty());
        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VerifyDeadline(scheduler, 1, 1000);
        const auto first = HeartbeatRequests(*connection).front();

        VERIFY_IS_TRUE(scheduler->AdvanceTo(600));
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(connection, EmptySuccessResponse(first)));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(3));
        VerifyDeadline(scheduler, 2, 1100);
        VERIFY_IS_TRUE(scheduler->AdvanceTo(1099));
        VERIFY_ARE_EQUAL(size_t{ 1 }, HeartbeatRequests(*connection).size());
        VERIFY_IS_TRUE(scheduler->AdvanceTo(1100));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 2));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(4));
        VerifyDeadline(scheduler, 3, 1600);

        const auto second = HeartbeatRequests(*connection).back();
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(connection, EmptySuccessResponse(second)));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(5));
        VerifyDeadline(scheduler, 4, 1600);
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 2100));
    }

    void TaskRuntimeClientHeartbeatIntegrationTests::MissingAckRetiresAtFullDeadlineThenBacksOff()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.Backoff->EnqueueDelay(100);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto first = std::make_shared<FakeRuntimeClientConnectionState>();
        auto second = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(first, HeartbeatHelloResponse());
        fixture.Connector->EnqueueSuccess(second, HeartbeatHelloResponse(), true);
        TaskRuntimeClient client{
            HeartbeatConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *first));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*first, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VerifyDeadline(scheduler, 1, 1000);

        VERIFY_IS_TRUE(scheduler->AdvanceTo(999));
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Ready);
        VERIFY_IS_TRUE(scheduler->AdvanceTo(1000));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(3));
        VerifyDeadline(scheduler, 2, 1100);
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Backoff);
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_IS_TRUE(CancelPrecedesClose(*first));

        VERIFY_IS_TRUE(scheduler->AdvanceTo(1099));
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_IS_TRUE(scheduler->AdvanceTo(1100));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        fixture.Connector->ReleaseConnect();
        VERIFY_IS_TRUE(WaitReady(client, *second));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(4));
        VerifyDeadline(scheduler, 3, 1600);
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 2100));
    }

    void TaskRuntimeClientHeartbeatIntegrationTests::AdvancePastFullDeadlineDoesNotSendLatePing()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.Backoff->EnqueueDelay(100);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, HeartbeatHelloResponse());
        TaskRuntimeClient client{
            HeartbeatConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        VERIFY_IS_TRUE(scheduler->AdvanceTo(1001));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VerifyDeadline(scheduler, 1, 1101);
        VERIFY_IS_TRUE(HeartbeatRequests(*connection).empty());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 1101));
    }

    void TaskRuntimeClientHeartbeatIntegrationTests::PublicResponseRearmsHeartbeatWindow()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, HeartbeatHelloResponse());
        TaskRuntimeClient client{
            HeartbeatConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        const auto request = client.Submit(ControlMethod::QueryTask);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 1));
        const auto outbound = connection->OutboundRequests().front();

        VERIFY_IS_TRUE(scheduler->AdvanceTo(400));
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(connection, EmptySuccessResponse(outbound)));
        VERIFY_IS_TRUE(WaitReady(request));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VerifyDeadline(scheduler, 1, 900);
        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_IS_TRUE(HeartbeatRequests(*connection).empty());
        VERIFY_IS_TRUE(scheduler->AdvanceTo(900));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(3));

        const auto heartbeat = HeartbeatRequests(*connection).front();
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(connection, EmptySuccessResponse(heartbeat)));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(4));
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 1900));
    }

    void TaskRuntimeClientHeartbeatIntegrationTests::SupersededOldAckDoesNotRefreshLiveness()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, HeartbeatHelloResponse());
        TaskRuntimeClient client{
            HeartbeatConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        const auto oldHeartbeat = HeartbeatRequests(*connection).front();

        const auto request = client.Submit(ControlMethod::QueryTask);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 2));
        const auto requests = connection->OutboundRequests();
        const auto user = std::ranges::find_if(requests, [](const auto& value) {
            return !IsReservedHeartbeatRequestId(value.RequestId);
        });
        VERIFY_IS_TRUE(user != requests.end());
        if (user == requests.end())
        {
            (void)StopWithBoundedSchedulerCleanup(client, scheduler, 1000);
            return;
        }

        VERIFY_IS_TRUE(scheduler->AdvanceTo(600));
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(connection, EmptySuccessResponse(*user)));
        VERIFY_IS_TRUE(WaitReady(request));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(3));
        VerifyDeadline(scheduler, 2, 1100);
        VERIFY_IS_TRUE(scheduler->AdvanceTo(900));
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(
            connection,
            EmptySuccessResponse(oldHeartbeat)));
        VERIFY_ARE_EQUAL(size_t{ 3 }, scheduler->WaitCallCount());

        VERIFY_IS_TRUE(scheduler->AdvanceTo(1099));
        VERIFY_ARE_EQUAL(size_t{ 1 }, HeartbeatRequests(*connection).size());
        VERIFY_IS_TRUE(scheduler->AdvanceTo(1100));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 2));
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Ready);
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(4));
        const auto current = HeartbeatRequests(*connection).back();
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(connection, EmptySuccessResponse(current)));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(5));
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 2100));
    }

    void TaskRuntimeClientHeartbeatIntegrationTests::SubscriptionResponseRearmsHeartbeatWindow()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, HeartbeatHelloResponse());
        TaskRuntimeClient client{
            HeartbeatConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        const auto subscription = client.Subscribe();
        VERIFY_IS_TRUE(subscription.Accepted);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 1));
        const auto snapshotRequest = connection->OutboundRequests().front();
        VERIFY_IS_TRUE(snapshotRequest.Method == ControlMethod::SnapshotAndSubscribe);

        VERIFY_IS_TRUE(scheduler->AdvanceTo(400));
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(
            connection,
            SnapshotResponse(snapshotRequest, CanonicalSubscriptionIds[0], 1, 1)));
        const auto snapshot = PullSubscriptionEvent(subscription.Stream);
        VERIFY_IS_TRUE(snapshot && snapshot->Kind == RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VerifyDeadline(scheduler, 1, 900);
        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_IS_TRUE(HeartbeatRequests(*connection).empty());
        VERIFY_IS_TRUE(scheduler->AdvanceTo(900));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(3));
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 1400));
    }

    void TaskRuntimeClientHeartbeatIntegrationTests::SubscriptionNotificationRearmsHeartbeatWindow()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, HeartbeatHelloResponse());
        TaskRuntimeClient client{
            HeartbeatConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        const auto subscription = client.Subscribe();
        VERIFY_IS_TRUE(subscription.Accepted);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 1));
        const auto snapshotRequest = connection->OutboundRequests().front();
        VERIFY_IS_TRUE(snapshotRequest.Method == ControlMethod::SnapshotAndSubscribe);
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(
            connection,
            SnapshotResponse(snapshotRequest, CanonicalSubscriptionIds[0], 1, 1)));
        const auto snapshot = PullSubscriptionEvent(subscription.Stream);
        VERIFY_IS_TRUE(snapshot && snapshot->Kind == RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));

        VERIFY_IS_TRUE(scheduler->AdvanceTo(400));
        VERIFY_IS_TRUE(QueueNotificationAndWaitProcessed(
            connection,
            SubscriptionNotification(CanonicalSubscriptionIds[0], 1, 2)));
        const auto delta = PullSubscriptionEvent(subscription.Stream);
        VERIFY_IS_TRUE(delta && delta->Kind == RuntimeClientSubscriptionEventKind::Notification);
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(3));
        VerifyDeadline(scheduler, 2, 900);
        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_IS_TRUE(HeartbeatRequests(*connection).empty());
        VERIFY_IS_TRUE(scheduler->AdvanceTo(900));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(4));
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 1400));
    }
}
