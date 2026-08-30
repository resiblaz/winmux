// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRuntimeClientScheduler.h"
#include "TaskRuntimeClientTestSupport.h"

#include "ProtocolRpcJson.h"
#include "RuntimeClientHeartbeat.h"

#include <algorithm>
#include <chrono>
#include <limits>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeClientLocalTests::TestSupport;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        constexpr std::string_view OtherClientInstanceId{
            "10000000-0000-4000-8000-000000000499"
        };

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

        std::optional<std::string> PayloadForEncodedSize(
            const size_t targetBytes,
            const ControlMethod method,
            const std::string_view requestId)
        {
            size_t low{};
            size_t high{ targetBytes };
            while (low <= high)
            {
                const auto padding = low + (high - low) / 2;
                std::string payload{ "{\"padding\":\"" };
                payload.append(padding, 'x');
                payload.append("\"}");
                ControlRequest request;
                request.RequestId = requestId;
                request.Method = method;
                request.PayloadJson = payload;
                const auto encoded = EncodeControlRequest(request);
                if (!encoded.Succeeded || encoded.Payload.size() > targetBytes)
                {
                    if (padding == 0)
                    {
                        break;
                    }
                    high = padding - 1;
                }
                else if (encoded.Payload.size() < targetBytes)
                {
                    low = padding + 1;
                }
                else
                {
                    return payload;
                }
            }
            return std::nullopt;
        }

        ControlResponse SuccessResponse(const std::string& requestId)
        {
            ControlResponse response;
            response.RequestId = requestId;
            response.Succeeded = true;
            response.ResultJson = "{}";
            return response;
        }

        class ReleaseEvent final
        {
        public:
            ReleaseEvent() noexcept : _event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) } {}
            ~ReleaseEvent()
            {
                if (_event)
                {
                    SetEvent(_event);
                    CloseHandle(_event);
                }
            }
            [[nodiscard]] bool Valid() const noexcept { return _event != nullptr; }
            [[nodiscard]] HANDLE Get() const noexcept { return _event; }
            void Signal() const noexcept { SetEvent(_event); }

        private:
            HANDLE _event{};
        };

        void VerifyReservedAckFaults(const std::string& heartbeatRequestId)
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
            const auto queued = client.Submit(ControlMethod::QueryTask);
            VERIFY_IS_TRUE(queued.Accepted);
            VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 1));
            VERIFY_IS_TRUE(connection->QueueInboundResponse(SuccessResponse(heartbeatRequestId)));
            VERIFY_IS_TRUE(WaitReady(queued));
            const auto result = queued.Completion.get();
            VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::ProtocolError);
            VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Faulted);
            VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
            VERIFY_IS_TRUE(StopAndWait(client));
        }
    }

    class TaskRuntimeClientHeartbeatFailureTests
    {
        TEST_CLASS(TaskRuntimeClientHeartbeatFailureTests);

        TEST_METHOD(HeartbeatSendsWhenUserByteBudgetIsExactlyFull);
        TEST_METHOD(HeartbeatAckMayCompleteBeforeSendReturns);
        TEST_METHOD(ActiveHeartbeatSchedulerFailureFaultsQueuedRequest);
        TEST_METHOD(HeartbeatSendFailureRetiresThenBacksOff);
        TEST_METHOD(WrongNamespaceAndFutureHeartbeatAcksFaultClient);
        TEST_METHOD(PublicReservedRequestIdIsRejectedBeforeWire);
    };

    void TaskRuntimeClientHeartbeatFailureTests::HeartbeatSendsWhenUserByteBudgetIsExactlyFull()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 2);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, HeartbeatHelloResponse());
        const auto configuration = HeartbeatConfiguration();
        const auto userByteBudget = configuration.MaximumQueuedBytes -
                                    configuration.MaximumRequestBytes -
                                    configuration.ReservedSubscriptionRequestBytes;
        const auto payload = PayloadForEncodedSize(
            userByteBudget,
            ControlMethod::QueryTask,
            CanonicalRequestIds[0]);
        VERIFY_IS_TRUE(payload.has_value());
        if (!payload)
        {
            return;
        }

        TaskRuntimeClient client{
            configuration,
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        const auto full = client.Submit(ControlMethod::QueryTask, *payload);
        VERIFY_IS_TRUE(full.Accepted);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 1));
        const auto rejected = client.Submit(ControlMethod::RuntimeStatus);
        VERIFY_IS_FALSE(rejected.Accepted);
        VERIFY_IS_TRUE(WaitReady(rejected));
        VERIFY_IS_TRUE(rejected.Completion.get().Outcome ==
                       RuntimeClientRequestOutcome::CapacityExceeded);

        VERIFY_IS_TRUE(scheduler->AdvanceTo(500));
        VERIFY_IS_TRUE(WaitForHeartbeatCount(*connection, 1));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(2));
        VERIFY_ARE_EQUAL(size_t{ 2 }, connection->OutboundRequests().size());
        VERIFY_IS_TRUE(full.Completion.wait_for(std::chrono::milliseconds{ 0 }) ==
                       std::future_status::timeout);
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 1000));
        VERIFY_IS_TRUE(WaitReady(full));
        VERIFY_IS_TRUE(full.Completion.get().Outcome == RuntimeClientRequestOutcome::ClientStopped);
    }

    void TaskRuntimeClientHeartbeatFailureTests::HeartbeatAckMayCompleteBeforeSendReturns()
    {
        RuntimeClientDependenciesFixture fixture;
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        ReleaseEvent releaseSendHook;
        VERIFY_IS_TRUE(releaseSendHook.Valid());
        connection->SetSendHook([event = releaseSendHook.Get()] {
            WaitForSingleObject(event, 2000);
        });
        connection->BlockNextSendAfterResponse();
        connection->SetNextAutoResponseHandler([](const ControlRequest& request) {
            return SuccessResponse(request.RequestId);
        });
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
        releaseSendHook.Signal();
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(
            connection->ResponseQueuedEvent(), 2000));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(3));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_TIMEOUT }, WaitForSingleObject(
            connection->SendExitedEvent(), 0));
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Ready);
        connection->ReleaseSend();
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(
            connection->SendExitedEvent(), 2000));
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 1500));
    }

    void TaskRuntimeClientHeartbeatFailureTests::ActiveHeartbeatSchedulerFailureFaultsQueuedRequest()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        VERIFY_IS_TRUE(scheduler->FailNextWait(ERROR_INVALID_HANDLE));
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, HeartbeatHelloResponse(), true);
        TaskRuntimeClient client{
            HeartbeatConfiguration(),
            DependenciesWithScheduler(fixture, scheduler)
        };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        const auto queued = client.Submit(ControlMethod::QueryTask);
        VERIFY_IS_TRUE(queued.Accepted);
        fixture.Connector->ReleaseConnect();
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(1));
        VERIFY_IS_TRUE(WaitReady(queued));
        const auto result = queued.Completion.get();
        VERIFY_IS_TRUE(result.Outcome == RuntimeClientRequestOutcome::ProtocolError);
        VERIFY_IS_TRUE(result.TransportStatus == RuntimeTransportStatus::SystemError);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, result.SystemError);
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Faulted);
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientHeartbeatFailureTests::HeartbeatSendFailureRetiresThenBacksOff()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.Backoff->EnqueueDelay(100);
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        auto first = std::make_shared<FakeRuntimeClientConnectionState>();
        auto second = std::make_shared<FakeRuntimeClientConnectionState>();
        ReleaseEvent releaseSendHook;
        VERIFY_IS_TRUE(releaseSendHook.Valid());
        first->SetSendHook([event = releaseSendHook.Get()] {
            WaitForSingleObject(event, 2000);
        });
        first->QueueSendFailure(
            RuntimeTransportStatus::Disconnected,
            "heartbeat send failed",
            ERROR_BROKEN_PIPE);
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
        releaseSendHook.Signal();
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(3));
        const auto deadlines = scheduler->ObservedDeadlines();
        VERIFY_IS_TRUE(deadlines.size() >= 3);
        if (deadlines.size() >= 3)
        {
            VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 600 }, deadlines[2]);
        }
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Backoff);
        VERIFY_IS_TRUE(CancelPrecedesClose(*first));
        VERIFY_IS_TRUE(scheduler->AdvanceTo(599));
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
        VERIFY_IS_TRUE(scheduler->AdvanceTo(600));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        fixture.Connector->ReleaseConnect();
        VERIFY_IS_TRUE(WaitReady(client, *second));
        VERIFY_IS_TRUE(scheduler->WaitForWaitEntered(4));
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 1100));
    }

    void TaskRuntimeClientHeartbeatFailureTests::WrongNamespaceAndFutureHeartbeatAcksFaultClient()
    {
        const auto ownNamespace = RuntimeClientHeartbeatNamespace(ClientInstanceId);
        const auto otherNamespace = RuntimeClientHeartbeatNamespace(OtherClientInstanceId);
        VERIFY_IS_TRUE(ownNamespace && otherNamespace && ownNamespace != otherNamespace);
        if (!ownNamespace || !otherNamespace || ownNamespace == otherNamespace)
        {
            return;
        }
        const auto wrongNamespace = MakeRuntimeClientHeartbeatRequestId(*otherNamespace, 1);
        const auto futureSequence = MakeRuntimeClientHeartbeatRequestId(*ownNamespace, 1);
        VERIFY_IS_TRUE(wrongNamespace && futureSequence);
        if (!wrongNamespace || !futureSequence)
        {
            return;
        }
        VerifyReservedAckFaults(*wrongNamespace);
        VerifyReservedAckFaults(*futureSequence);
    }

    void TaskRuntimeClientHeartbeatFailureTests::PublicReservedRequestIdIsRejectedBeforeWire()
    {
        RuntimeClientDependenciesFixture fixture;
        const auto requestNamespace = RuntimeClientHeartbeatNamespace(ClientInstanceId);
        const auto reserved = requestNamespace ?
                                  MakeRuntimeClientHeartbeatRequestId(*requestNamespace, 1) :
                                  std::nullopt;
        VERIFY_IS_TRUE(reserved.has_value());
        if (!reserved)
        {
            return;
        }
        fixture.RequestIds->Enqueue(*reserved);
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
        const auto rejected = client.Submit(ControlMethod::QueryTask);
        VERIFY_IS_FALSE(rejected.Accepted);
        VERIFY_IS_TRUE(WaitReady(rejected));
        VERIFY_IS_TRUE(rejected.Completion.get().Outcome ==
                       RuntimeClientRequestOutcome::ProtocolError);
        VERIFY_IS_TRUE(connection->OutboundRequests().empty());
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Ready);
        VERIFY_IS_TRUE(StopWithBoundedSchedulerCleanup(client, scheduler, 500));
    }
}
