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

        class DeadlineReleaseGuard final
        {
        public:
            explicit DeadlineReleaseGuard(
                std::shared_ptr<FakeRuntimeClientScheduler> scheduler) noexcept :
                _scheduler{ std::move(scheduler) }
            {
            }

            ~DeadlineReleaseGuard()
            {
                Release();
            }

            void Release() noexcept
            {
                if (_scheduler)
                {
                    _scheduler->ReleaseDeadline();
                    _scheduler.reset();
                }
            }

        private:
            std::shared_ptr<FakeRuntimeClientScheduler> _scheduler;
        };
    }

    class TaskRuntimeClientHeartbeatLifecycleTests
    {
        TEST_CLASS(TaskRuntimeClientHeartbeatLifecycleTests);

        TEST_METHOD(HeartbeatUsesReservedCapacityWhenUserQueueIsFull);
        TEST_METHOD(StopWinsWhileHeartbeatDeadlineReturns);
        TEST_METHOD(StopCancelsInflightHeartbeatWithoutReconnect);
        TEST_METHOD(HeartbeatAckResetsReconnectBackoffHealth);
    };

    void TaskRuntimeClientHeartbeatLifecycleTests::HeartbeatUsesReservedCapacityWhenUserQueueIsFull()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 3);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, HeartbeatHelloResponse());
        auto configuration = HeartbeatConfiguration();
        configuration.MaximumOperations = 2;
        TaskRuntimeClient client{
            configuration,
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));

        const auto first = client.Submit(ControlMethod::QueryTask, "{\"slot\":1}");
        const auto second = client.Submit(ControlMethod::RuntimeStatus, "{\"slot\":2}");
        VERIFY_IS_TRUE(first.Accepted && second.Accepted);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 2));
        const auto rejected = client.Submit(ControlMethod::QueryTask, "{\"slot\":3}");
        VERIFY_IS_FALSE(rejected.Accepted);
        VERIFY_IS_TRUE(WaitReady(rejected));
        VERIFY_IS_TRUE(rejected.Completion.get().Outcome ==
                       RuntimeClientRequestOutcome::CapacityExceeded);

        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VERIFY_ARE_EQUAL(size_t{ 3 }, connection->OutboundRequests().size());
        VERIFY_IS_TRUE(first.Completion.wait_for(std::chrono::milliseconds{ 0 }) ==
                       std::future_status::timeout);
        VERIFY_IS_TRUE(second.Completion.wait_for(std::chrono::milliseconds{ 0 }) ==
                       std::future_status::timeout);

        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 1000));
        VERIFY_IS_TRUE(WaitReady(first) && WaitReady(second));
        VERIFY_IS_TRUE(first.Completion.get().Outcome == RuntimeClientRequestOutcome::ClientStopped);
        VERIFY_IS_TRUE(second.Completion.get().Outcome == RuntimeClientRequestOutcome::ClientStopped);
    }

    void TaskRuntimeClientHeartbeatLifecycleTests::StopWinsWhileHeartbeatDeadlineReturns()
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
        VERIFY_IS_TRUE(scheduler->BlockNextDeadlineReturn());
        DeadlineReleaseGuard release{ scheduler };
        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, scheduler->WaitForDeadlineBlocked());

        const auto completedBefore = CompletedWaits(scheduler);
        client.RequestStop();
        release.Release();
        auto completed = scheduler->WaitForCompletedWaits(completedBefore + 1);
        if (!completed)
        {
            (void)scheduler->AdvanceTo(1000);
            scheduler->Pulse();
            completed = scheduler->WaitForCompletedWaits(completedBefore + 1);
        }
        const auto stopped = client.WaitForStopped();
        VERIFY_IS_TRUE(completed && stopped);
        VERIFY_ARE_EQUAL(size_t{ 1 }, scheduler->SignalWakeCount());
        VERIFY_IS_TRUE(HeartbeatRequests(*connection).empty());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
    }

    void TaskRuntimeClientHeartbeatLifecycleTests::StopCancelsInflightHeartbeatWithoutReconnect()
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
        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VerifyDeadline(scheduler, 1, 1000);

        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 1000));
        VERIFY_ARE_EQUAL(size_t{ 1 }, HeartbeatRequests(*connection).size());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientHeartbeatLifecycleTests::HeartbeatAckResetsReconnectBackoffHealth()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.Backoff->EnqueueDelay(100);
        fixture.Backoff->EnqueueDelay(200);
        fixture.Backoff->EnqueueDelay(100);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto first = std::make_shared<FakeRuntimeClientConnectionState>();
        auto second = std::make_shared<FakeRuntimeClientConnectionState>();
        auto healthy = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(first, HeartbeatHelloResponse());
        fixture.Connector->EnqueueSuccess(
            second,
            HeartbeatHelloResponse("20000000-0000-4000-8000-000000000405"));
        fixture.Connector->EnqueueSuccess(
            healthy,
            HeartbeatHelloResponse("20000000-0000-4000-8000-000000000406"));
        TaskRuntimeClient client{
            HeartbeatConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *first));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        first->QueueInboundFailure(
            RuntimeTransportStatus::Disconnected,
            "flap one",
            ERROR_BROKEN_PIPE);
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VerifyDeadline(scheduler, 1, 100);

        VERIFY_IS_TRUE(scheduler->AdvanceTo(100));
        VERIFY_IS_TRUE(WaitReady(client, *second));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(3));
        VerifyDeadline(scheduler, 2, 600);
        second->QueueInboundFailure(
            RuntimeTransportStatus::Disconnected,
            "flap two",
            ERROR_BROKEN_PIPE);
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(4));
        VerifyDeadline(scheduler, 3, 300);

        VERIFY_IS_TRUE(scheduler->AdvanceTo(300));
        VERIFY_IS_TRUE(WaitReady(client, *healthy));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(5));
        VerifyDeadline(scheduler, 4, 800);
        VERIFY_IS_TRUE(scheduler->AdvanceTo(800));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*healthy, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(6));
        const auto heartbeat = HeartbeatRequests(*healthy).front();
        VERIFY_IS_TRUE(scheduler->AdvanceTo(850));
        VERIFY_IS_TRUE(QueueResponseAndWaitProcessed(healthy, EmptySuccessResponse(heartbeat)));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(7));
        VerifyDeadline(scheduler, 6, 1350);

        healthy->QueueInboundFailure(
            RuntimeTransportStatus::Disconnected,
            "after heartbeat health",
            ERROR_BROKEN_PIPE);
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(8));
        VerifyDeadline(scheduler, 7, 950);
        const auto failures = fixture.Backoff->ObservedFailures();
        VERIFY_ARE_EQUAL(size_t{ 3 }, failures.size());
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, failures[0]);
        VERIFY_ARE_EQUAL(uint32_t{ 2 }, failures[1]);
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, failures[2]);
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 950));
        VERIFY_IS_TRUE(CancelPrecedesClose(*first));
        VERIFY_IS_TRUE(CancelPrecedesClose(*second));
        VERIFY_IS_TRUE(CancelPrecedesClose(*healthy));
    }
}
