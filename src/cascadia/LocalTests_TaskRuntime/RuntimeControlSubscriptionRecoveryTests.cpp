// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeControlNotificationTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::ControlFixtures;
using namespace TaskRuntimeLocalTests::NotificationFixtures;

namespace TaskRuntimeLocalTests
{
    class RuntimeControlSubscriptionRecoveryTests
    {
        TEST_CLASS(RuntimeControlSubscriptionRecoveryTests);

        TEST_METHOD(ConflictingPlanDoesNotRemoveExistingOwner);
        TEST_METHOD(RouterExceptionAfterPlanningCancelsReservation);
    };

    void RuntimeControlSubscriptionRecoveryTests::ConflictingPlanDoesNotRemoveExistingOwner()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 1, 0);
        auto client = ConnectAndHello(server, ClientOneId);
        Subscribe(client, "30000000-0000-4000-8000-000000000071");

        const auto conflict = Request(
            client,
            "30000000-0000-4000-8000-000000000072",
            ControlMethod::SnapshotAndSubscribe);
        VERIFY_IS_FALSE(conflict.Succeeded);
        VERIFY_IS_TRUE(conflict.Error.has_value());
        VERIFY_IS_TRUE(conflict.Error->Code == ProtocolRpcErrorCode::Conflict);
        VERIFY_ARE_EQUAL(size_t{ 1 }, server.Router->Calls.load(std::memory_order_relaxed));
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            server.Router->CancelledPlans.load(std::memory_order_relaxed));

        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 1, 1)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, ReceiveNotification(client).Sequence);
    }

    void RuntimeControlSubscriptionRecoveryTests::RouterExceptionAfterPlanningCancelsReservation()
    {
        RunningControlServer server;
        server.Router->SetSubscriptionPlanner([](
                                                  const RuntimeControlClientContext&,
                                                  const ControlRequest&,
                                                  const RuntimeControlCancellationToken&) {
            return std::optional{ RuntimeControlSubscriptionPlan{
                std::string{ SubscriptionOneId },
                1,
            } };
        });
        auto client = ConnectAndHello(server, ClientOneId);
        const auto failed = Request(
            client,
            "30000000-0000-4000-8000-000000000073",
            ControlMethod::SnapshotAndSubscribe,
            "{\"throw\":true}");
        VERIFY_IS_FALSE(failed.Succeeded);
        VERIFY_IS_TRUE(failed.Error.has_value());
        VERIFY_IS_TRUE(failed.Error->Code == ProtocolRpcErrorCode::InternalError);
        VERIFY_ARE_EQUAL(size_t{ 1 }, server.Router->Calls.load(std::memory_order_relaxed));
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            server.Router->CancelledPlans.load(std::memory_order_relaxed));
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 1, 1)) ==
                       RuntimeControlPublishStatus::NoSubscriber);
    }
}
