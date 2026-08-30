// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeControlNotificationTestFixtures.h"

#include <atomic>
#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::ControlFixtures;
using namespace TaskRuntimeLocalTests::NotificationFixtures;

namespace TaskRuntimeLocalTests
{
    class RuntimeControlNotificationConcurrencyTests
    {
        TEST_CLASS(RuntimeControlNotificationConcurrencyTests);

        TEST_METHOD(PendingDuplicateOwnerIsVisibleBeforeRouteReturns);
        TEST_METHOD(PendingBudgetReservesSnapshotResponseSlot);
        TEST_METHOD(StopRejectsEveryPublishAfterItsLinearizationPoint);
    };

    void RuntimeControlNotificationConcurrencyTests::PendingDuplicateOwnerIsVisibleBeforeRouteReturns()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 1, 0);
        const auto secondEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const auto allowSecond = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const auto publishDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY_IS_NOT_NULL(secondEntered);
        VERIFY_IS_NOT_NULL(allowSecond);
        VERIFY_IS_NOT_NULL(publishDone);
        std::atomic<DWORD> handlerWait{ WAIT_FAILED };
        server.Router->SetHandler([&](
                                      const RuntimeControlClientContext& client,
                                      const ControlRequest& request,
                                      const RuntimeControlCancellationToken&) {
            if (request.Method != ControlMethod::SnapshotAndSubscribe)
            {
                return RouteSuccess();
            }
            if (client.ClientInstanceId == ClientTwoId)
            {
                SetEvent(secondEntered);
                handlerWait.store(
                    WaitForSingleObject(allowSecond, 5000),
                    std::memory_order_release);
            }
            return Bind(SubscriptionOneId, 1, 0);
        });
        auto first = ConnectAndHello(server, ClientOneId);
        auto second = ConnectAndHello(server, ClientTwoId);
        Subscribe(first, "30000000-0000-4000-8000-000000000081");
        SendRequest(
            second,
            "30000000-0000-4000-8000-000000000082",
            ControlMethod::SnapshotAndSubscribe);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(secondEntered, 5000));

        const auto notification = Notification(SubscriptionOneId, 1, 1);
        std::atomic<RuntimeControlPublishStatus> publishStatus{
            RuntimeControlPublishStatus::NoSubscriber
        };
        std::thread publisher{ [&] {
            publishStatus.store(
                server.Server.PublishNotification(notification),
                std::memory_order_release);
            SetEvent(publishDone);
        } };
        const auto publishWait = WaitForSingleObject(publishDone, 1000);
        if (publishWait != WAIT_OBJECT_0)
        {
            SetEvent(allowSecond);
        }
        publisher.join();
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, publishWait);
        VERIFY_IS_TRUE(publishStatus.load(std::memory_order_acquire) ==
                       RuntimeControlPublishStatus::AmbiguousSubscription);
        SetEvent(allowSecond);
        VERIFY_IS_TRUE(ReceiveResponse(second).Succeeded);
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            handlerWait.load(std::memory_order_acquire));
        VERIFY_IS_TRUE(server.Server.PublishNotification(notification) ==
                       RuntimeControlPublishStatus::AmbiguousSubscription);
        CloseHandle(secondEntered);
        CloseHandle(allowSecond);
        CloseHandle(publishDone);
    }

    void RuntimeControlNotificationConcurrencyTests::StopRejectsEveryPublishAfterItsLinearizationPoint()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 1, 0);
        auto client = ConnectAndHello(server, ClientOneId);
        Subscribe(client, "30000000-0000-4000-8000-000000000083");

        std::thread stopper{ [&] { server.Server.Stop(); } };
        bool observedStopped{};
        uint64_t sequence{ 1 };
        const auto deadline = GetTickCount64() + 3000;
        while (!observedStopped && GetTickCount64() < deadline)
        {
            const auto status = server.Server.PublishNotification(
                Notification(SubscriptionOneId, 1, sequence++));
            observedStopped = status == RuntimeControlPublishStatus::ServerStopped;
            SwitchToThread();
        }
        stopper.join();
        VERIFY_IS_TRUE(observedStopped);
        for (size_t attempt = 0; attempt < 20; ++attempt)
        {
            VERIFY_IS_TRUE(server.Server.PublishNotification(
                               Notification(SubscriptionOneId, 1, sequence++)) ==
                           RuntimeControlPublishStatus::ServerStopped);
        }
    }

    void RuntimeControlNotificationConcurrencyTests::PendingBudgetReservesSnapshotResponseSlot()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.MaximumQueuedMessages = 3;
        configuration.MaximumQueuedBytes = 512 * 1024;
        RunningControlServer server{ configuration };
        ConfigureSubscription(server.Router, SubscriptionOneId, 1, 0);
        std::array<std::atomic<RuntimeControlPublishStatus>, 2> statuses;
        for (auto& status : statuses)
        {
            status.store(RuntimeControlPublishStatus::NoSubscriber, std::memory_order_relaxed);
        }
        server.Router->SetHandler([&](
                                      const RuntimeControlClientContext&,
                                      const ControlRequest& request,
                                      const RuntimeControlCancellationToken&) {
            if (request.Method == ControlMethod::SnapshotAndSubscribe)
            {
                statuses[0].store(
                    server.Server.PublishNotification(Notification(SubscriptionOneId, 1, 1)),
                    std::memory_order_release);
                statuses[1].store(
                    server.Server.PublishNotification(Notification(SubscriptionOneId, 1, 2)),
                    std::memory_order_release);
                return Bind(SubscriptionOneId, 1, 0);
            }
            return RouteSuccess();
        });
        auto client = ConnectAndHello(server, ClientOneId);
        Subscribe(client, "30000000-0000-4000-8000-000000000084");
        VERIFY_IS_TRUE(statuses[0].load(std::memory_order_acquire) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_IS_TRUE(statuses[1].load(std::memory_order_acquire) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, ReceiveNotification(client).Sequence);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, ReceiveNotification(client).Sequence);
    }
}
