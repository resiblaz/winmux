// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlTestFixtures.h"

namespace TaskRuntimeLocalTests::NotificationFixtures
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;
    using namespace TaskRuntimeLocalTests::ControlFixtures;

    inline constexpr std::string_view SubscriptionOneId{ "40000000-0000-4000-8000-000000000041" };
    inline constexpr std::string_view SubscriptionTwoId{ "40000000-0000-4000-8000-000000000042" };

    inline RuntimeControlRouteResult RouteSuccess()
    {
        RuntimeControlRouteResult result;
        result.Succeeded = true;
        return result;
    }

    inline RuntimeControlRouteResult Bind(
        const std::string_view subscriptionId,
        const uint64_t generation,
        const uint64_t cursor)
    {
        auto result = RouteSuccess();
        result.ResultJson =
            "{\"subscriptionId\":\"" + std::string{ subscriptionId } +
            "\",\"generation\":" + std::to_string(generation) +
            ",\"cursor\":" + std::to_string(cursor) +
            ",\"snapshot\":{}}";
        result.SubscriptionChange = RuntimeControlSubscriptionChange{
            RuntimeControlSubscriptionAction::Bind,
            std::string{ subscriptionId },
            generation,
            cursor,
        };
        return result;
    }

    inline RuntimeControlRouteResult Unbind(
        const std::string_view subscriptionId,
        const uint64_t generation)
    {
        auto result = RouteSuccess();
        result.SubscriptionChange = RuntimeControlSubscriptionChange{
            RuntimeControlSubscriptionAction::Unbind,
            std::string{ subscriptionId },
            generation,
            0,
        };
        return result;
    }

    inline void ConfigureSubscription(
        const std::shared_ptr<FakeControlRouter>& router,
        const std::string_view subscriptionId,
        const uint64_t generation = 1,
        const uint64_t cursor = 0)
    {
        const std::string id{ subscriptionId };
        router->SetSubscriptionPlanner([id, generation](
                                           const RuntimeControlClientContext&,
                                           const ControlRequest&,
                                           const RuntimeControlCancellationToken&) {
            return std::optional{ RuntimeControlSubscriptionPlan{ id, generation } };
        });
        router->SetHandler([id, generation, cursor](
                               const RuntimeControlClientContext&,
                               const ControlRequest& request,
                               const RuntimeControlCancellationToken&) {
            if (request.Method == ControlMethod::SnapshotAndSubscribe)
            {
                return Bind(id, generation, cursor);
            }
            if (request.Method == ControlMethod::Unsubscribe)
            {
                return Unbind(id, generation);
            }
            return RouteSuccess();
        });
    }

    inline ControlNotification Notification(
        const std::string_view subscriptionId,
        const uint64_t generation,
        const uint64_t sequence,
        const ControlNotificationKind kind = ControlNotificationKind::ProjectionChanged,
        std::string payload = "{}")
    {
        ControlNotification notification;
        notification.SubscriptionId = subscriptionId;
        notification.Generation = generation;
        notification.Sequence = sequence;
        notification.Kind = kind;
        notification.PayloadJson = std::move(payload);
        return notification;
    }

    inline ControlNotification ReceiveNotification(
        RuntimeControlChannel& channel,
        const uint32_t timeoutMilliseconds = 5000)
    {
        const auto received = channel.Receive(timeoutMilliseconds);
        VERIFY_IS_TRUE(received.Succeeded());
        const auto decoded = DecodeControlNotification(received.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        return decoded.Value;
    }

    inline void Subscribe(
        RuntimeControlChannel& client,
        const std::string_view requestId)
    {
        const auto response = Request(
            client,
            requestId,
            ControlMethod::SnapshotAndSubscribe);
        VERIFY_IS_TRUE(response.Succeeded);
    }
}
