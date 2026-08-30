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
    class RuntimeControlNotificationTests
    {
        TEST_CLASS(RuntimeControlNotificationTests);

        TEST_METHOD(SnapshotResponsePrecedesFirstNotification);
        TEST_METHOD(NotificationTargetsOnlyOwningWindow);
        TEST_METHOD(UnsubscribeStopsFutureDelivery);
        TEST_METHOD(SuccessfulSubscriptionMethodsRequireMatchingSideEffects);
        TEST_METHOD(EnforcesMonotonicNotificationSequence);
        TEST_METHOD(SubscriptionClosedRemovesOwnership);
        TEST_METHOD(SlowSubscriberDoesNotBlockAnotherSubscription);
        TEST_METHOD(DisconnectCleansOwnershipAndReconnects);
        TEST_METHOD(InvalidSubscriptionChangeReturnsInternalError);
        TEST_METHOD(InvalidOrWrongGenerationNotificationIsNotDelivered);
        TEST_METHOD(NegotiatedPayloadLimitAppliesBeforeQueueing);
        TEST_METHOD(SubscriptionCountIsBoundedPerClient);
        TEST_METHOD(AmbiguousSubscriptionOwnerFailsClosed);
    };

    void RuntimeControlNotificationTests::SnapshotResponsePrecedesFirstNotification()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 2, 10);
        const auto notification = Notification(SubscriptionOneId, 2, 11);
        std::atomic<RuntimeControlPublishStatus> publishStatus{
            RuntimeControlPublishStatus::NoSubscriber
        };
        server.Router->SetHandler([&](
                                      const RuntimeControlClientContext&,
                                      const ControlRequest& request,
                                      const RuntimeControlCancellationToken&) {
            if (request.Method == ControlMethod::SnapshotAndSubscribe)
            {
                publishStatus.store(
                    server.Server.PublishNotification(notification),
                    std::memory_order_release);
                return Bind(SubscriptionOneId, 2, 10);
            }
            return RouteSuccess();
        });
        auto client = ConnectAndHello(server, ClientOneId);
        const auto response = Request(
            client,
            "30000000-0000-4000-8000-000000000051",
            ControlMethod::SnapshotAndSubscribe);
        VERIFY_IS_TRUE(response.Succeeded);
        VERIFY_IS_TRUE(publishStatus.load(std::memory_order_acquire) ==
                       RuntimeControlPublishStatus::Delivered);
        const auto delivered = ReceiveNotification(client);
        VERIFY_ARE_EQUAL(uint64_t{ 11 }, delivered.Sequence);
        VERIFY_ARE_EQUAL(std::string{ SubscriptionOneId }, delivered.SubscriptionId);
    }

    void RuntimeControlNotificationTests::NotificationTargetsOnlyOwningWindow()
    {
        RunningControlServer server;
        server.Router->SetSubscriptionPlanner([](
                                                  const RuntimeControlClientContext& client,
                                                  const ControlRequest&,
                                                  const RuntimeControlCancellationToken&) {
            return std::optional{ RuntimeControlSubscriptionPlan{
                client.ClientInstanceId == ClientOneId ?
                    std::string{ SubscriptionOneId } :
                    std::string{ SubscriptionTwoId },
                1,
            } };
        });
        server.Router->SetHandler([](
                                      const RuntimeControlClientContext& client,
                                      const ControlRequest& request,
                                      const RuntimeControlCancellationToken&) {
            if (request.Method != ControlMethod::SnapshotAndSubscribe)
            {
                return RouteSuccess();
            }
            return client.ClientInstanceId == ClientOneId ?
                       Bind(SubscriptionOneId, 1, 0) :
                       Bind(SubscriptionTwoId, 1, 0);
        });
        auto first = ConnectAndHello(server, ClientOneId);
        auto second = ConnectAndHello(server, ClientTwoId);
        Subscribe(first, "30000000-0000-4000-8000-000000000052");
        Subscribe(second, "30000000-0000-4000-8000-000000000053");

        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 1, 1)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(std::string{ SubscriptionOneId }, ReceiveNotification(first).SubscriptionId);
        VERIFY_IS_TRUE(second.Receive(50).Status == RuntimeTransportStatus::Timeout);

        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionTwoId, 1, 1)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(std::string{ SubscriptionTwoId }, ReceiveNotification(second).SubscriptionId);
    }

    void RuntimeControlNotificationTests::UnsubscribeStopsFutureDelivery()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 3, 0);
        auto client = ConnectAndHello(server, ClientOneId);
        Subscribe(client, "30000000-0000-4000-8000-000000000054");
        const auto response = Request(
            client,
            "30000000-0000-4000-8000-000000000055",
            ControlMethod::Unsubscribe);
        VERIFY_IS_TRUE(response.Succeeded);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 3, 1)) ==
                       RuntimeControlPublishStatus::NoSubscriber);
    }

    void RuntimeControlNotificationTests::SuccessfulSubscriptionMethodsRequireMatchingSideEffects()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 3, 0);
        server.Router->SetHandler([](
                                      const RuntimeControlClientContext&,
                                      const ControlRequest&,
                                      const RuntimeControlCancellationToken&) {
            return RouteSuccess();
        });
        auto client = ConnectAndHello(server, ClientOneId);
        const auto missingBind = Request(
            client,
            "30000000-0000-4000-8000-000000000072",
            ControlMethod::SnapshotAndSubscribe);
        VERIFY_IS_FALSE(missingBind.Succeeded);
        VERIFY_IS_TRUE(missingBind.Error.has_value());
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 3, 1)) ==
                       RuntimeControlPublishStatus::NoSubscriber);

        server.Router->SetHandler([](
                                      const RuntimeControlClientContext&,
                                      const ControlRequest& request,
                                      const RuntimeControlCancellationToken&) {
            return request.Method == ControlMethod::SnapshotAndSubscribe ?
                       Bind(SubscriptionOneId, 3, 0) :
                       RouteSuccess();
        });
        Subscribe(client, "30000000-0000-4000-8000-000000000073");
        const auto missingUnbind = Request(
            client,
            "30000000-0000-4000-8000-000000000074",
            ControlMethod::Unsubscribe);
        VERIFY_IS_FALSE(missingUnbind.Succeeded);
        VERIFY_IS_TRUE(missingUnbind.Error.has_value());
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 3, 1)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, ReceiveNotification(client).Sequence);
    }

    void RuntimeControlNotificationTests::EnforcesMonotonicNotificationSequence()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 4, 10);
        auto client = ConnectAndHello(server, ClientOneId);
        Subscribe(client, "30000000-0000-4000-8000-000000000056");

        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 4, 11)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 4, 11)) ==
                       RuntimeControlPublishStatus::Duplicate);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 4, 13)) ==
                       RuntimeControlPublishStatus::SequenceGap);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 4, 12)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(uint64_t{ 11 }, ReceiveNotification(client).Sequence);
        VERIFY_ARE_EQUAL(uint64_t{ 12 }, ReceiveNotification(client).Sequence);
        VERIFY_IS_TRUE(client.Receive(50).Status == RuntimeTransportStatus::Timeout);
    }

    void RuntimeControlNotificationTests::SubscriptionClosedRemovesOwnership()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 5, 0);
        auto client = ConnectAndHello(server, ClientOneId);
        Subscribe(client, "30000000-0000-4000-8000-000000000057");

        VERIFY_IS_TRUE(server.Server.PublishNotification(Notification(
                           SubscriptionOneId,
                           5,
                           1,
                           ControlNotificationKind::SubscriptionClosed)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 5, 2)) ==
                       RuntimeControlPublishStatus::NoSubscriber);
        const auto closed = ReceiveNotification(client);
        VERIFY_IS_TRUE(closed.Kind == ControlNotificationKind::SubscriptionClosed);
    }

    void RuntimeControlNotificationTests::SlowSubscriberDoesNotBlockAnotherSubscription()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 50;
        configuration.WriteTimeoutMilliseconds = 1000;
        configuration.MaximumQueuedMessages = 1;
        configuration.MaximumQueuedBytes = 512 * 1024;
        RunningControlServer server{ configuration };
        server.Router->SetSubscriptionPlanner([](
                                                  const RuntimeControlClientContext& client,
                                                  const ControlRequest&,
                                                  const RuntimeControlCancellationToken&) {
            return std::optional{ RuntimeControlSubscriptionPlan{
                client.ClientInstanceId == ClientOneId ?
                    std::string{ SubscriptionOneId } :
                    std::string{ SubscriptionTwoId },
                1,
            } };
        });
        server.Router->SetHandler([](
                                      const RuntimeControlClientContext& client,
                                      const ControlRequest& request,
                                      const RuntimeControlCancellationToken&) {
            if (request.Method != ControlMethod::SnapshotAndSubscribe)
            {
                return RouteSuccess();
            }
            return client.ClientInstanceId == ClientOneId ?
                       Bind(SubscriptionOneId, 1, 0) :
                       Bind(SubscriptionTwoId, 1, 0);
        });
        auto slow = ConnectAndHello(server, ClientOneId);
        auto fast = ConnectAndHello(server, ClientTwoId);
        Subscribe(slow, "30000000-0000-4000-8000-000000000058");
        Subscribe(fast, "30000000-0000-4000-8000-000000000059");

        const auto largePayload = "{\"padding\":\"" + std::string(128 * 1024, 'x') + "\"}";
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 1, 1, ControlNotificationKind::ProjectionChanged, largePayload)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 1, 2, ControlNotificationKind::ProjectionChanged, largePayload)) ==
                       RuntimeControlPublishStatus::SlowClientDisconnected);

        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionTwoId, 1, 1)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, ReceiveNotification(fast).Sequence);
        VERIFY_IS_TRUE(WaitUntil([&] {
            return server.Server.Statistics().SlowClientDisconnects == 1;
        }, 5000));
    }

    void RuntimeControlNotificationTests::DisconnectCleansOwnershipAndReconnects()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 50;
        RunningControlServer server{ configuration };
        server.Router->SetSubscriptionPlanner([](
                                                  const RuntimeControlClientContext& client,
                                                  const ControlRequest&,
                                                  const RuntimeControlCancellationToken&) {
            return std::optional{ RuntimeControlSubscriptionPlan{
                std::string{ SubscriptionOneId },
                client.ClientInstanceId == ClientOneId ? 1ull : 2ull,
            } };
        });
        server.Router->SetHandler([](
                                      const RuntimeControlClientContext& client,
                                      const ControlRequest& request,
                                      const RuntimeControlCancellationToken&) {
            if (request.Method != ControlMethod::SnapshotAndSubscribe)
            {
                return RouteSuccess();
            }
            return client.ClientInstanceId == ClientOneId ?
                       Bind(SubscriptionOneId, 1, 0) :
                       Bind(SubscriptionOneId, 2, 0);
        });
        auto first = ConnectAndHello(server, ClientOneId);
        Subscribe(first, "30000000-0000-4000-8000-000000000061");
        first.Close();
        VERIFY_IS_TRUE(WaitUntil([&] {
            return server.Server.Statistics().CompletedConnections == 1;
        }));
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 1, 1)) ==
                       RuntimeControlPublishStatus::NoSubscriber);

        auto reconnected = ConnectAndHello(server, ClientTwoId);
        Subscribe(reconnected, "30000000-0000-4000-8000-000000000062");
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 2, 1)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, ReceiveNotification(reconnected).Generation);
    }

    void RuntimeControlNotificationTests::InvalidSubscriptionChangeReturnsInternalError()
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
        server.Router->SetHandler([](
                                      const RuntimeControlClientContext&,
                                      const ControlRequest&,
                                      const RuntimeControlCancellationToken&) {
            return Bind("not-a-subscription", 1, 0);
        });
        auto client = ConnectAndHello(server, ClientOneId);
        const auto response = Request(
            client,
            "30000000-0000-4000-8000-000000000063",
            ControlMethod::SnapshotAndSubscribe);
        VERIFY_IS_FALSE(response.Succeeded);
        VERIFY_IS_TRUE(response.Error.has_value());
        VERIFY_IS_TRUE(response.Error->Code == ProtocolRpcErrorCode::InternalError);
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            server.Router->CancelledPlans.load(std::memory_order_relaxed));
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 1, 1)) ==
                       RuntimeControlPublishStatus::NoSubscriber);
    }

    void RuntimeControlNotificationTests::InvalidOrWrongGenerationNotificationIsNotDelivered()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 7, 0);
        auto client = ConnectAndHello(server, ClientOneId);
        Subscribe(client, "30000000-0000-4000-8000-000000000068");

        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 7, 0)) ==
                       RuntimeControlPublishStatus::InvalidNotification);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 8, 1)) ==
                       RuntimeControlPublishStatus::NoSubscriber);
        VERIFY_IS_TRUE(client.Receive(50).Status == RuntimeTransportStatus::Timeout);
    }

    void RuntimeControlNotificationTests::NegotiatedPayloadLimitAppliesBeforeQueueing()
    {
        RunningControlServer server;
        ConfigureSubscription(server.Router, SubscriptionOneId, 9, 0);
        server.Router->SetHandler([](
                                      const RuntimeControlClientContext&,
                                      const ControlRequest& request,
                                      const RuntimeControlCancellationToken&) {
            if (request.Method == ControlMethod::SnapshotAndSubscribe)
            {
                return Bind(SubscriptionOneId, 9, 0);
            }
            auto result = RouteSuccess();
            result.ResultJson = "{\"padding\":\"" + std::string(2048, 'x') + "\"}";
            return result;
        });
        auto client = ConnectAndHello(
            server,
            ClientOneId,
            AllProtocolCapabilities,
            1024);
        const auto oversizedResponse = Request(
            client,
            "30000000-0000-4000-8000-000000000075",
            ControlMethod::Ping);
        VERIFY_IS_FALSE(oversizedResponse.Succeeded);
        VERIFY_IS_TRUE(oversizedResponse.Error.has_value());
        Subscribe(client, "30000000-0000-4000-8000-000000000076");

        const auto largePayload = "{\"padding\":\"" + std::string(2048, 'x') + "\"}";
        VERIFY_IS_TRUE(server.Server.PublishNotification(Notification(
                           SubscriptionOneId,
                           9,
                           1,
                           ControlNotificationKind::ProjectionChanged,
                           largePayload)) == RuntimeControlPublishStatus::InvalidNotification);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 9, 1)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, ReceiveNotification(client).Sequence);
    }

    void RuntimeControlNotificationTests::SubscriptionCountIsBoundedPerClient()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.MaximumSubscriptionsPerClient = 1;
        RunningControlServer server{ configuration };
        server.Router->SetSubscriptionPlanner([](
                                                  const RuntimeControlClientContext&,
                                                  const ControlRequest& request,
                                                  const RuntimeControlCancellationToken&) {
            return std::optional{ RuntimeControlSubscriptionPlan{
                request.PayloadJson.find("\"second\":true") == std::string::npos ?
                    std::string{ SubscriptionOneId } :
                    std::string{ SubscriptionTwoId },
                1,
            } };
        });
        server.Router->SetHandler([](
                                      const RuntimeControlClientContext&,
                                      const ControlRequest& request,
                                      const RuntimeControlCancellationToken&) {
            return request.PayloadJson.find("\"second\":true") == std::string::npos ?
                       Bind(SubscriptionOneId, 1, 0) :
                       Bind(SubscriptionTwoId, 1, 0);
        });
        auto client = ConnectAndHello(server, ClientOneId);
        const auto first = Request(
            client,
            "30000000-0000-4000-8000-000000000064",
            ControlMethod::SnapshotAndSubscribe);
        const auto second = Request(
            client,
            "30000000-0000-4000-8000-000000000065",
            ControlMethod::SnapshotAndSubscribe,
            "{\"second\":true}");
        VERIFY_IS_TRUE(first.Succeeded);
        VERIFY_IS_FALSE(second.Succeeded);
        VERIFY_IS_TRUE(second.Error.has_value());
        VERIFY_IS_TRUE(second.Error->Code == ProtocolRpcErrorCode::TooManyRequests);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionOneId, 1, 1)) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_IS_TRUE(server.Server.PublishNotification(
                           Notification(SubscriptionTwoId, 1, 1)) ==
                       RuntimeControlPublishStatus::NoSubscriber);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, ReceiveNotification(client).Sequence);
    }

    void RuntimeControlNotificationTests::AmbiguousSubscriptionOwnerFailsClosed()
    {
        RuntimeControlServerConfiguration configuration;
        configuration.AcceptTimeoutMilliseconds = 50;
        RunningControlServer server{ configuration };
        ConfigureSubscription(server.Router, SubscriptionOneId, 1, 0);
        auto first = ConnectAndHello(server, ClientOneId);
        auto second = ConnectAndHello(server, ClientTwoId);
        Subscribe(first, "30000000-0000-4000-8000-000000000066");
        Subscribe(second, "30000000-0000-4000-8000-000000000067");

        const auto notification = Notification(SubscriptionOneId, 1, 1);
        VERIFY_IS_TRUE(server.Server.PublishNotification(notification) ==
                       RuntimeControlPublishStatus::AmbiguousSubscription);
        VERIFY_IS_TRUE(first.Receive(50).Status == RuntimeTransportStatus::Timeout);
        VERIFY_IS_TRUE(second.Receive(50).Status == RuntimeTransportStatus::Timeout);

        first.Close();
        VERIFY_IS_TRUE(WaitUntil([&] {
            return server.Server.Statistics().CompletedConnections == 1;
        }));
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            server.Router->DisconnectedClients.load(std::memory_order_relaxed));
        VERIFY_IS_TRUE(server.Server.PublishNotification(notification) ==
                       RuntimeControlPublishStatus::Delivered);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, ReceiveNotification(second).Sequence);
    }
}
