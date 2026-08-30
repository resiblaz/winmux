// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeClientTestSupport.h"

#include <atomic>
#include <memory>
#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeClientLocalTests::TestSupport;

namespace TaskRuntimeClientLocalTests
{
    class TaskRuntimeClientStopTests
    {
        TEST_CLASS(TaskRuntimeClientStopTests);

        TEST_METHOD(StopDuringConnectCompletesQueuedRequest);
        TEST_METHOD(StopDuringBackoffIsImmediate);
        TEST_METHOD(StopDuringReceiveJoinsBeforeClose);
        TEST_METHOD(StopDuringBlockedMutationSendReturnsOutcomeUnknown);
        TEST_METHOD(CloseWaitsForCancellationToReturn);
        TEST_METHOD(FacadeMayBeDestroyedFromConnectCall);
        TEST_METHOD(FacadeMayBeDestroyedFromSendCall);
        TEST_METHOD(FacadeMayBeDestroyedFromReceiveCall);
        TEST_METHOD(FacadeMayBeDestroyedFromCancelCall);
        TEST_METHOD(FacadeMayBeDestroyedFromCloseCall);
        TEST_METHOD(ResponseRetireAndStopRaceCompletesExactlyOnce);
    };

    void TaskRuntimeClientStopTests::StopDuringConnectCompletesQueuedRequest()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse(), true);

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectEntered());
        const auto request = client.Submit(ControlMethod::QueryTask);
        VERIFY_IS_TRUE(request.Accepted);
        client.RequestStop();
        VERIFY_IS_TRUE(client.WaitForStopped());
        VERIFY_IS_TRUE(WaitReady(request));
        VERIFY_IS_TRUE(request.Completion.get().Outcome == RuntimeClientRequestOutcome::ClientStopped);
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Stopped);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectExited());
    }

    void TaskRuntimeClientStopTests::StopDuringBackoffIsImmediate()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        fixture.Backoff = std::make_shared<ScriptedRuntimeClientBackoffPolicy>(60000);
        fixture.Connector->EnqueueFailure(
            RuntimeTransportStatus::Timeout,
            RuntimeControlConnectPhase::Connecting,
            false,
            "runtime unavailable",
            HandshakeError::None,
            WAIT_TIMEOUT);

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitUntil([&] {
            return client.State() == TaskRuntimeClientState::Backoff &&
                   !fixture.Backoff->ObservedFailures().empty();
        }));
        const auto request = client.Submit(ControlMethod::RuntimeStatus);
        VERIFY_IS_TRUE(request.Accepted);
        const auto started = GetTickCount64();
        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_IS_TRUE(GetTickCount64() - started < 1000);
        VERIFY_IS_TRUE(WaitReady(request));
        VERIFY_IS_TRUE(request.Completion.get().Outcome == RuntimeClientRequestOutcome::ClientStopped);
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Connector->Calls().size());
    }

    void TaskRuntimeClientStopTests::StopDuringReceiveJoinsBeforeClose()
    {
        RuntimeClientDependenciesFixture fixture;
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connection->ReceiveEnteredEvent(), 2000));
        VERIFY_IS_TRUE(StopAndWait(client));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connection->ReceiveExitedEvent(), 2000));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientStopTests::StopDuringBlockedMutationSendReturnsOutcomeUnknown()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        connection->BlockNextSendAfterResponse();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));
        const auto request = client.Submit(ControlMethod::SubmitCommand);
        VERIFY_IS_TRUE(request.Accepted);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connection->SendEnteredEvent(), 2000));
        client.RequestStop();
        VERIFY_IS_TRUE(client.WaitForStopped());
        VERIFY_IS_TRUE(WaitReady(request));
        VERIFY_IS_TRUE(request.Completion.get().Outcome == RuntimeClientRequestOutcome::OutcomeUnknown);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connection->SendExitedEvent(), 2000));
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
    }

    void TaskRuntimeClientStopTests::FacadeMayBeDestroyedFromConnectCall()
    {
        RuntimeClientDependenciesFixture fixture;
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        const auto allowDestroy = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY_IS_NOT_NULL(allowDestroy);
        std::atomic<std::shared_ptr<TaskRuntimeClient>> client{
            std::make_shared<TaskRuntimeClient>(ValidConfiguration(), fixture.Dependencies())
        };
        fixture.Connector->SetConnectHook([&] {
            WaitForSingleObject(allowDestroy, 2000);
            client.store({}, std::memory_order_release);
        });

        {
            const auto owner = client.load(std::memory_order_acquire);
            VERIFY_IS_TRUE(owner->Start());
        }
        SetEvent(allowDestroy);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, fixture.Connector->WaitForConnectExited());
        VERIFY_IS_TRUE(WaitUntil([&] { return !client.load(std::memory_order_acquire); }));
        CloseHandle(allowDestroy);
    }

    void TaskRuntimeClientStopTests::CloseWaitsForCancellationToReturn()
    {
        RuntimeClientDependenciesFixture fixture;
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        connection->BlockNextCancel();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client));

        const auto stopReturned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY_IS_NOT_NULL(stopReturned);
        std::thread stopper{ [&] {
            client.RequestStop();
            SetEvent(stopReturned);
        } };
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(connection->CancelEnteredEvent(), 2000));
        const auto requestStopReturnedBeforeCancel = WaitForSingleObject(stopReturned, 1000);
        const auto lifecycleWhileCancelBlocked = connection->Lifecycle();
        connection->ReleaseCancel();
        stopper.join();

        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, requestStopReturnedBeforeCancel);
        VERIFY_IS_TRUE(std::ranges::find(
                           lifecycleWhileCancelBlocked,
                           FakeConnectionLifecycleAction::Close) == lifecycleWhileCancelBlocked.end());
        VERIFY_IS_TRUE(client.WaitForStopped());
        VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
        CloseHandle(stopReturned);
    }

    void TaskRuntimeClientStopTests::FacadeMayBeDestroyedFromSendCall()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        const auto allowDestroy = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY_IS_NOT_NULL(allowDestroy);
        std::atomic<std::shared_ptr<TaskRuntimeClient>> client{
            std::make_shared<TaskRuntimeClient>(ValidConfiguration(), fixture.Dependencies())
        };
        connection->SetSendHook([&] {
            WaitForSingleObject(allowDestroy, 2000);
            client.store({}, std::memory_order_release);
        });

        RuntimeClientRequestHandle request;
        {
            const auto owner = client.load(std::memory_order_acquire);
            VERIFY_IS_TRUE(owner->Start());
            VERIFY_IS_TRUE(WaitReady(*owner));
            request = owner->Submit(ControlMethod::Ping);
        }
        SetEvent(allowDestroy);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connection->SendExitedEvent(), 2000));
        VERIFY_IS_TRUE(WaitUntil([&] { return !client.load(std::memory_order_acquire); }));
        VERIFY_IS_TRUE(WaitReady(request));
        VERIFY_IS_TRUE(request.Completion.get().Outcome == RuntimeClientRequestOutcome::ClientStopped);
        VERIFY_IS_TRUE(WaitUntil([&] { return CancelPrecedesClose(*connection); }));
        CloseHandle(allowDestroy);
    }

    void TaskRuntimeClientStopTests::FacadeMayBeDestroyedFromReceiveCall()
    {
        RuntimeClientDependenciesFixture fixture;
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        const auto allowDestroy = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY_IS_NOT_NULL(allowDestroy);
        std::atomic<std::shared_ptr<TaskRuntimeClient>> client{
            std::make_shared<TaskRuntimeClient>(ValidConfiguration(), fixture.Dependencies())
        };
        connection->SetReceiveHook([&] {
            WaitForSingleObject(allowDestroy, 2000);
            client.store({}, std::memory_order_release);
        });

        {
            const auto owner = client.load(std::memory_order_acquire);
            VERIFY_IS_TRUE(owner->Start());
        }
        SetEvent(allowDestroy);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(connection->ReceiveExitedEvent(), 2000));
        VERIFY_IS_TRUE(WaitUntil([&] { return !client.load(std::memory_order_acquire); }));
        VERIFY_IS_TRUE(WaitUntil([&] { return CancelPrecedesClose(*connection); }));
        CloseHandle(allowDestroy);
    }

    void TaskRuntimeClientStopTests::ResponseRetireAndStopRaceCompletesExactlyOnce()
    {
        constexpr size_t iterations{ 40 };
        for (size_t iteration = 0; iteration < iterations; ++iteration)
        {
            RuntimeClientDependenciesFixture fixture;
            fixture.RequestIds->Enqueue(std::string{ CanonicalRequestIds[iteration % CanonicalRequestIds.size()] });
            auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
            fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
            TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
            VERIFY_IS_TRUE(client.Start());
            VERIFY_IS_TRUE(WaitReady(client));
            const auto request = client.Submit(ControlMethod::SubmitCommand);
            VERIFY_IS_TRUE(WaitUntil([&] { return connection->OutboundRequests().size() == 1; }));
            const auto requestId = connection->OutboundRequests()[0].RequestId;

            std::thread response{ [&] {
                ControlResponse value;
                value.RequestId = requestId;
                value.Succeeded = true;
                connection->QueueInboundResponse(value);
            } };
            std::thread failure{ [&] {
                connection->QueueInboundFailure(
                    RuntimeTransportStatus::Disconnected,
                    "racing disconnect",
                    ERROR_BROKEN_PIPE);
            } };
            std::thread stop{ [&] { client.RequestStop(); } };
            response.join();
            failure.join();
            stop.join();

            VERIFY_IS_TRUE(client.WaitForStopped());
            VERIFY_IS_TRUE(WaitReady(request));
            const auto outcome = request.Completion.get().Outcome;
            VERIFY_IS_TRUE(outcome == RuntimeClientRequestOutcome::Succeeded ||
                           outcome == RuntimeClientRequestOutcome::OutcomeUnknown);
            VERIFY_IS_TRUE(CancelPrecedesClose(*connection));
        }
    }

    void TaskRuntimeClientStopTests::FacadeMayBeDestroyedFromCancelCall()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        std::atomic<std::shared_ptr<TaskRuntimeClient>> client{
            std::make_shared<TaskRuntimeClient>(ValidConfiguration(), fixture.Dependencies())
        };
        connection->SetCancelHook([&] {
            client.store({}, std::memory_order_release);
        });

        RuntimeClientRequestHandle request;
        {
            const auto owner = client.load(std::memory_order_acquire);
            VERIFY_IS_TRUE(owner->Start());
            VERIFY_IS_TRUE(WaitReady(*owner));
            request = owner->Submit(ControlMethod::SubmitCommand);
            VERIFY_IS_TRUE(WaitUntil([&] { return connection->OutboundRequests().size() == 1; }));
        }
        connection->QueueInboundFailure(
            RuntimeTransportStatus::Disconnected,
            "cancel reentrancy",
            ERROR_BROKEN_PIPE);

        VERIFY_IS_TRUE(WaitUntil([&] { return !client.load(std::memory_order_acquire); }));
        VERIFY_IS_TRUE(WaitReady(request));
        VERIFY_IS_TRUE(request.Completion.get().Outcome == RuntimeClientRequestOutcome::OutcomeUnknown);
        VERIFY_IS_TRUE(WaitUntil([&] { return CancelPrecedesClose(*connection); }));
    }

    void TaskRuntimeClientStopTests::FacadeMayBeDestroyedFromCloseCall()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 1);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        std::atomic<std::shared_ptr<TaskRuntimeClient>> client{
            std::make_shared<TaskRuntimeClient>(ValidConfiguration(), fixture.Dependencies())
        };
        connection->SetCloseHook([&] {
            client.store({}, std::memory_order_release);
        });

        RuntimeClientRequestHandle request;
        {
            const auto owner = client.load(std::memory_order_acquire);
            VERIFY_IS_TRUE(owner->Start());
            VERIFY_IS_TRUE(WaitReady(*owner));
            request = owner->Submit(ControlMethod::SubmitCommand);
            VERIFY_IS_TRUE(WaitUntil([&] { return connection->OutboundRequests().size() == 1; }));
        }
        connection->QueueInboundFailure(
            RuntimeTransportStatus::Disconnected,
            "close reentrancy",
            ERROR_BROKEN_PIPE);

        VERIFY_IS_TRUE(WaitUntil([&] { return !client.load(std::memory_order_acquire); }));
        VERIFY_IS_TRUE(WaitReady(request));
        VERIFY_IS_TRUE(request.Completion.get().Outcome == RuntimeClientRequestOutcome::OutcomeUnknown);
        VERIFY_IS_TRUE(WaitUntil([&] { return CancelPrecedesClose(*connection); }));
    }
}
