// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRuntimeClientScheduler.h"
#include "TaskRuntimeClientTestSupport.h"

#include <chrono>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeClientLocalTests::TestSupport;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        TaskRuntimeClientDependencies DependenciesWithScheduler(
            const RuntimeClientDependenciesFixture& fixture,
            const std::shared_ptr<FakeRuntimeClientScheduler>& scheduler)
        {
            auto dependencies = fixture.Dependencies();
            dependencies.Scheduler = scheduler;
            return dependencies;
        }

        void EnqueueTransientConnectFailure(
            const std::shared_ptr<FakeRuntimeClientConnector>& connector,
            const bool block = false)
        {
            connector->EnqueueFailure(
                RuntimeTransportStatus::Timeout,
                RuntimeControlConnectPhase::Connecting,
                block,
                "runtime unavailable",
                HandshakeError::None,
                WAIT_TIMEOUT);
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
    }

    class TaskRuntimeClientSchedulerTests
    {
        TEST_CLASS(TaskRuntimeClientSchedulerTests);

        TEST_METHOD(ReconnectOccursAtExactVirtualDeadline);
        TEST_METHOD(FrozenVirtualTimeDoesNotBusyLoop);
        TEST_METHOD(StopWakesBackoffWithoutAdvancingTime);
        TEST_METHOD(SchedulerFailureFaultsQueuedRequestWithoutRetry);
        TEST_METHOD(ZeroBackoffFaultsWithoutScheduling);
        TEST_METHOD(SaturatedClockFaultsWithoutReconnect);
        TEST_METHOD(RequestQueuedDuringBackoffSucceedsAfterDeadline);
        TEST_METHOD(ConsecutiveBackoffsUseAbsoluteDeadlines);
    };

    void TaskRuntimeClientSchedulerTests::ReconnectOccursAtExactVirtualDeadline()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.Backoff->EnqueueDelay(100);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connected = std::make_shared<FakeRuntimeClientConnectionState>();
        EnqueueTransientConnectFailure(fixture.Connector);
        fixture.Connector->EnqueueSuccess(connected, ValidHelloResponse(), true);

        TaskRuntimeClient client{
            ValidConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        VerifyDeadline(scheduler, 0, 100);
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());

        scheduler->Pulse();
        VERIFY_IS_TRUE(scheduler->AdvanceTo(99));
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_ARE_EQUAL(size_t{ 0 }, scheduler->DeadlineWakeCount());
        VERIFY_ARE_EQUAL(size_t{ 1 }, scheduler->PendingWaitCount());

        VERIFY_IS_TRUE(scheduler->AdvanceTo(100));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        VERIFY_ARE_EQUAL(size_t{ 2 }, fixture.Connector->Calls().size());
        fixture.Connector->ReleaseConnect();
        VERIFY_IS_TRUE(WaitReady(client, *connected));
        VERIFY_ARE_EQUAL(size_t{ 1 }, scheduler->DeadlineWakeCount());
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSchedulerTests::FrozenVirtualTimeDoesNotBusyLoop()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.Backoff->EnqueueDelay(100);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto unused = std::make_shared<FakeRuntimeClientConnectionState>();
        EnqueueTransientConnectFailure(fixture.Connector);
        fixture.Connector->EnqueueSuccess(unused, ValidHelloResponse(), true);

        TaskRuntimeClient client{
            ValidConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        for (size_t pulse = 0; pulse < 16; ++pulse)
        {
            scheduler->Pulse();
            VERIFY_IS_TRUE(scheduler->AdvanceTo(0));
        }

        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_ARE_EQUAL(size_t{ 1 }, scheduler->WaitCallCount());
        VERIFY_ARE_EQUAL(size_t{ 1 }, scheduler->PendingWaitCount());
        VERIFY_ARE_EQUAL(size_t{ 0 }, scheduler->DeadlineWakeCount());
        client.RequestStop();
        const auto stoppedWithoutClockAdvance = scheduler->WaitForCompletedWaits(1);
        if (!stoppedWithoutClockAdvance)
        {
            (void)scheduler->AdvanceTo(100);
        }
        VERIFY_IS_TRUE(client.WaitForStopped());
        VERIFY_IS_TRUE(stoppedWithoutClockAdvance);
        VERIFY_ARE_EQUAL(size_t{ 1 }, scheduler->SignalWakeCount());
    }

    void TaskRuntimeClientSchedulerTests::StopWakesBackoffWithoutAdvancingTime()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        fixture.Backoff->EnqueueDelay(60000);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>(500);
        EnqueueTransientConnectFailure(fixture.Connector);

        TaskRuntimeClient client{
            ValidConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        VerifyDeadline(scheduler, 0, 60500);
        const auto request = client.Submit(ControlMethod::RuntimeStatus);
        VERIFY_IS_TRUE(request.Accepted);

        client.RequestStop();
        const auto stoppedWithoutClockAdvance = scheduler->WaitForCompletedWaits(1);
        if (!stoppedWithoutClockAdvance)
        {
            (void)scheduler->AdvanceTo(60500);
        }
        VERIFY_IS_TRUE(client.WaitForStopped());
        VERIFY_IS_TRUE(stoppedWithoutClockAdvance);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 500 }, scheduler->Now());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_ARE_EQUAL(size_t{ 0 }, scheduler->DeadlineWakeCount());
        VERIFY_ARE_EQUAL(size_t{ 1 }, scheduler->SignalWakeCount());
        VERIFY_IS_TRUE(WaitReady(request));
        VERIFY_IS_TRUE(request.Completion.get().Outcome ==
                       RuntimeClientRequestOutcome::ClientStopped);
    }

    void TaskRuntimeClientSchedulerTests::SchedulerFailureFaultsQueuedRequestWithoutRetry()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        fixture.Backoff->EnqueueDelay(100);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        VERIFY_IS_TRUE(scheduler->FailNextWait(ERROR_INVALID_HANDLE));
        EnqueueTransientConnectFailure(fixture.Connector, true);

        TaskRuntimeClient client{
            ValidConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        const auto request = client.Submit(ControlMethod::QueryTask);
        const auto subscription = client.Subscribe();
        VERIFY_IS_TRUE(request.Accepted);
        VERIFY_IS_TRUE(subscription.Accepted);
        fixture.Connector->ReleaseConnect();

        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        VERIFY_IS_TRUE(WaitReady(request));
        const auto result = request.Completion.get();
        VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::ProtocolError);
        VERIFY_IS_TRUE(result.TransportStatus == RuntimeTransportStatus::SystemError);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, result.SystemError);
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Faulted);
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_ARE_EQUAL(size_t{ 1 }, scheduler->FailedWaitCount());
        const auto subscriptionFault = PullSubscriptionEvent(subscription.Stream);
        VERIFY_IS_TRUE(subscriptionFault.has_value());
        VERIFY_IS_TRUE(subscriptionFault &&
                       subscriptionFault->Kind == RuntimeClientSubscriptionEventKind::Faulted);
        VERIFY_IS_TRUE(subscriptionFault &&
                       subscriptionFault->TransportStatus == RuntimeTransportStatus::SystemError);
        VERIFY_IS_TRUE(subscription.Stream->Pull(0).Status ==
                       RuntimeClientSubscriptionPullStatus::Closed);
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSchedulerTests::ZeroBackoffFaultsWithoutScheduling()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        fixture.Backoff->EnqueueDelay(0);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        EnqueueTransientConnectFailure(fixture.Connector, true);

        TaskRuntimeClient client{
            ValidConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        const auto request = client.Submit(ControlMethod::RuntimeStatus);
        VERIFY_IS_TRUE(request.Accepted);
        fixture.Connector->ReleaseConnect();

        VERIFY_IS_TRUE(WaitReady(request));
        const auto result = request.Completion.get();
        VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::ProtocolError);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_PARAMETER }, result.SystemError);
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Faulted);
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_ARE_EQUAL(size_t{ 0 }, scheduler->WaitCallCount());
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSchedulerTests::SaturatedClockFaultsWithoutReconnect()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        fixture.Backoff->EnqueueDelay(2);
        constexpr auto maximum =
            (std::numeric_limits<RuntimeClientMonotonicMilliseconds>::max)();
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>(maximum - 1);
        EnqueueTransientConnectFailure(fixture.Connector, true);

        TaskRuntimeClient client{
            ValidConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        const auto request = client.Submit(ControlMethod::RuntimeStatus);
        VERIFY_IS_TRUE(request.Accepted);
        fixture.Connector->ReleaseConnect();

        VERIFY_IS_TRUE(WaitReady(request));
        const auto result = request.Completion.get();
        VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::ProtocolError);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_ARITHMETIC_OVERFLOW }, result.SystemError);
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Faulted);
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_ARE_EQUAL(size_t{ 0 }, scheduler->WaitCallCount());
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSchedulerTests::RequestQueuedDuringBackoffSucceedsAfterDeadline()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        fixture.Backoff->EnqueueDelay(100);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connected = std::make_shared<FakeRuntimeClientConnectionState>();
        connected->EnableAutoResponse();
        EnqueueTransientConnectFailure(fixture.Connector);
        fixture.Connector->EnqueueSuccess(connected, ValidHelloResponse());

        TaskRuntimeClient client{
            ValidConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        const auto request = client.Submit(ControlMethod::QueryTask, "{\"taskId\":\"queued\"}");
        VERIFY_IS_TRUE(request.Accepted);
        VERIFY_IS_TRUE(request.Completion.wait_for(std::chrono::milliseconds{ 0 }) ==
                       std::future_status::timeout);
        VERIFY_IS_TRUE(connected->OutboundRequests().empty());

        VERIFY_IS_TRUE(scheduler->AdvanceTo(100));
        VERIFY_IS_TRUE(WaitReady(client, *connected));
        VERIFY_IS_TRUE(WaitReady(request));
        const auto result = request.Completion.get();
        VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::Succeeded);
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, result.Attempts);
        VERIFY_ARE_EQUAL(std::string{ CanonicalRequestIds[0] }, result.LastRequestId);
        VERIFY_ARE_EQUAL(size_t{ 1 }, connected->OutboundRequests().size());
        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connected));
    }

    void TaskRuntimeClientSchedulerTests::ConsecutiveBackoffsUseAbsoluteDeadlines()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.Backoff->EnqueueDelay(100);
        fixture.Backoff->EnqueueDelay(200);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>(1000);
        auto connected = std::make_shared<FakeRuntimeClientConnectionState>();
        EnqueueTransientConnectFailure(fixture.Connector);
        EnqueueTransientConnectFailure(fixture.Connector);
        fixture.Connector->EnqueueSuccess(connected, ValidHelloResponse(), true);

        TaskRuntimeClient client{
            ValidConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        VerifyDeadline(scheduler, 0, 1100);
        VERIFY_IS_TRUE(scheduler->AdvanceTo(1100));

        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VerifyDeadline(scheduler, 1, 1300);
        VERIFY_ARE_EQUAL(size_t{ 2 }, fixture.Connector->Calls().size());
        VERIFY_IS_TRUE(scheduler->AdvanceTo(1299));
        VERIFY_ARE_EQUAL(size_t{ 2 }, fixture.Connector->Calls().size());

        VERIFY_IS_TRUE(scheduler->AdvanceTo(1300));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        VERIFY_ARE_EQUAL(size_t{ 3 }, fixture.Connector->Calls().size());
        fixture.Connector->ReleaseConnect();
        VERIFY_IS_TRUE(WaitReady(client, *connected));
        const auto failures = fixture.Backoff->ObservedFailures();
        VERIFY_ARE_EQUAL(size_t{ 2 }, failures.size());
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, failures[0]);
        VERIFY_ARE_EQUAL(uint32_t{ 2 }, failures[1]);
        VERIFY_ARE_EQUAL(size_t{ 2 }, scheduler->DeadlineWakeCount());
        VERIFY_IS_TRUE(StopAndWait(client));
    }
}
