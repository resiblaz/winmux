// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "FakeRuntimeClientConnector.h"
#include "FakeRuntimeClientServices.h"

#include "TaskRuntimeClient.h"

#include "ProtocolCapabilities.h"
#include "ProtocolVersion.h"
#include "SnapshotAndSubscribe.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <future>
#include <string>
#include <string_view>

namespace TaskRuntimeClientLocalTests::TestSupport
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    inline constexpr std::string_view RuntimeProtocolIdentity{ "wt-task-runtime-v1:test-client" };
    inline constexpr std::string_view ClientInstanceId{ "10000000-0000-4000-8000-000000000301" };
    inline constexpr std::string_view RuntimeInstanceId{ "20000000-0000-4000-8000-000000000301" };

    inline constexpr std::array<std::string_view, 16> CanonicalRequestIds{
        "30000000-0000-4000-8000-000000000301",
        "30000000-0000-4000-8000-000000000302",
        "30000000-0000-4000-8000-000000000303",
        "30000000-0000-4000-8000-000000000304",
        "30000000-0000-4000-8000-000000000305",
        "30000000-0000-4000-8000-000000000306",
        "30000000-0000-4000-8000-000000000307",
        "30000000-0000-4000-8000-000000000308",
        "30000000-0000-4000-8000-000000000309",
        "30000000-0000-4000-8000-000000000310",
        "30000000-0000-4000-8000-000000000311",
        "30000000-0000-4000-8000-000000000312",
        "30000000-0000-4000-8000-000000000313",
        "30000000-0000-4000-8000-000000000314",
        "30000000-0000-4000-8000-000000000315",
        "30000000-0000-4000-8000-000000000316",
    };
    inline constexpr std::array<std::string_view, 8> CanonicalSubscriptionIds{
        "40000000-0000-4000-8000-000000000301",
        "40000000-0000-4000-8000-000000000302",
        "40000000-0000-4000-8000-000000000303",
        "40000000-0000-4000-8000-000000000304",
        "40000000-0000-4000-8000-000000000305",
        "40000000-0000-4000-8000-000000000306",
        "40000000-0000-4000-8000-000000000307",
        "40000000-0000-4000-8000-000000000308",
    };

    [[nodiscard]] inline ControlResponse SnapshotResponse(
        const ControlRequest& request,
        const std::string_view subscriptionId,
        const uint64_t generation,
        const uint64_t cursor,
        std::string snapshotJson = "{}")
    {
        SnapshotAndSubscribeResult snapshot;
        snapshot.SubscriptionId = subscriptionId;
        snapshot.Generation = generation;
        snapshot.Cursor = cursor;
        snapshot.SnapshotJson = std::move(snapshotJson);
        const auto encoded = EncodeSnapshotAndSubscribeResult(snapshot);
        ControlResponse response;
        response.RequestId = request.RequestId;
        response.Succeeded = encoded.Succeeded;
        response.ResultJson = encoded.Succeeded ? encoded.Payload : "{}";
        return response;
    }

    [[nodiscard]] inline ControlResponse EmptySuccessResponse(const ControlRequest& request)
    {
        ControlResponse response;
        response.RequestId = request.RequestId;
        response.Succeeded = true;
        return response;
    }

    [[nodiscard]] inline ControlNotification SubscriptionNotification(
        const std::string_view subscriptionId,
        const uint64_t generation,
        const uint64_t sequence,
        const ControlNotificationKind kind = ControlNotificationKind::ProjectionChanged,
        std::string payloadJson = "{}")
    {
        ControlNotification notification;
        notification.SubscriptionId = subscriptionId;
        notification.Generation = generation;
        notification.Sequence = sequence;
        notification.Kind = kind;
        notification.PayloadJson = std::move(payloadJson);
        return notification;
    }

    inline void InstallSubscriptionResponder(
        const std::shared_ptr<FakeRuntimeClientConnectionState>& connection,
        const std::string_view subscriptionId,
        const uint64_t generation,
        const uint64_t cursor,
        std::string snapshotJson = "{}")
    {
        connection->SetAutoResponseHandler(
            [subscriptionId = std::string{ subscriptionId },
             generation,
             cursor,
             snapshotJson = std::move(snapshotJson)](const ControlRequest& request) {
                return request.Method == ControlMethod::SnapshotAndSubscribe ?
                           SnapshotResponse(request, subscriptionId, generation, cursor, snapshotJson) :
                           EmptySuccessResponse(request);
            });
    }

    inline void QueueUnknownSubscriptionNotification(
        const std::shared_ptr<FakeRuntimeClientConnectionState>& connection,
        const std::string_view subscriptionId,
        const uint64_t generation,
        const uint64_t sequence,
        const std::string_view unknownKind,
        const std::string_view payloadJson = "{}")
    {
        std::string payload;
        payload.reserve(192 + payloadJson.size());
        payload.append("{\"type\":\"notification\",\"subscriptionId\":\"");
        payload.append(subscriptionId);
        payload.append("\",\"generation\":");
        payload.append(std::to_string(generation));
        payload.append(",\"sequence\":");
        payload.append(std::to_string(sequence));
        payload.append(",\"event\":\"");
        payload.append(unknownKind);
        payload.append("\",\"payload\":");
        payload.append(payloadJson);
        payload.push_back('}');
        connection->QueueInboundPayload(std::move(payload));
    }

    [[nodiscard]] inline TaskRuntimeClientConfiguration ValidConfiguration()
    {
        TaskRuntimeClientConfiguration configuration;
        configuration.Identity.ProtocolIdentity = std::string{ RuntimeProtocolIdentity };
        configuration.Identity.ControlPipeName = LR"(\\.\pipe\WT.TaskRuntime.test-client.control)";
        configuration.Hello.ClientInstanceId = std::string{ ClientInstanceId };
        configuration.Hello.ClientKind = ProtocolClientKind::TestPeer;
        configuration.Hello.ExpectedRuntimeIdentity = std::string{ RuntimeProtocolIdentity };
        configuration.Hello.OptionalCapabilities = AllProtocolCapabilities;
        configuration.Hello.MaximumAcceptedControlFrameBytes = 256 * 1024;
        configuration.ConnectTimeoutMilliseconds = 2000;
        configuration.SendTimeoutMilliseconds = 2000;
        configuration.ReceiveTimeoutMilliseconds = 50;
        configuration.MaximumOperations = 32;
        configuration.MaximumSubscriptions = 8;
        configuration.MaximumConcurrentSubscriptionOperations = 2;
        configuration.MaximumTombstones = 128;
        configuration.MaximumQueuedBytes = 1024 * 1024;
        configuration.ReservedSubscriptionRequestBytes = 512 * 1024;
        configuration.MaximumAggregateSubscriptionEventBytes = 8 * 1024 * 1024;
        configuration.MaximumRequestBytes = 256 * 1024;
        configuration.MaximumRememberedRequestIds = 256;
        configuration.DefaultMaximumAttempts = 3;
        configuration.MaximumAttemptsPerOperation = 8;
        configuration.MaximumSubscriptionAttempts = 3;
        configuration.MaximumDiagnosticBytes = 4096;
        return configuration;
    }

    [[nodiscard]] inline HelloResponse ValidHelloResponse(
        const std::string_view runtimeInstanceId = RuntimeInstanceId)
    {
        HelloResponse response;
        response.RuntimeIdentity = std::string{ RuntimeProtocolIdentity };
        response.RuntimeInstanceId = std::string{ runtimeInstanceId };
        response.ProtocolMajor = CurrentProtocolMajor;
        response.ProtocolMinor = CurrentProtocolMinor;
        response.Capabilities = AllProtocolCapabilities;
        response.MaximumControlFrameBytes = 256 * 1024;
        response.MaximumTerminalFrameBytes = 64 * 1024;
        response.HeartbeatMilliseconds = 60000;
        return response;
    }

    struct RuntimeClientDependenciesFixture
    {
        std::shared_ptr<FakeRuntimeClientConnector> Connector{
            std::make_shared<FakeRuntimeClientConnector>()
        };
        std::shared_ptr<DeterministicRuntimeClientRequestIdSource> RequestIds{
            std::make_shared<DeterministicRuntimeClientRequestIdSource>()
        };
        std::shared_ptr<ScriptedRuntimeClientBackoffPolicy> Backoff{
            std::make_shared<ScriptedRuntimeClientBackoffPolicy>()
        };

        [[nodiscard]] TaskRuntimeClientDependencies Dependencies() const
        {
            return { Connector, RequestIds, Backoff };
        }

        void EnqueueRequestIds(
            const size_t first = 0,
            const size_t count = CanonicalRequestIds.size()) const
        {
            const auto begin = (std::min)(first, CanonicalRequestIds.size());
            const auto end = begin + (std::min)(count, CanonicalRequestIds.size() - begin);
            for (auto index = begin; index < end; ++index)
            {
                RequestIds->Enqueue(std::string{ CanonicalRequestIds[index] });
            }
        }
    };

    template<typename TPredicate>
    [[nodiscard]] bool WaitUntil(
        TPredicate&& predicate,
        const uint32_t timeoutMilliseconds = 2000,
        const uint32_t pollMilliseconds = 5)
    {
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        while (true)
        {
            if (predicate())
            {
                return true;
            }
            const auto now = GetTickCount64();
            if (now >= deadline)
            {
                return predicate();
            }
            Sleep((std::min)(pollMilliseconds, static_cast<uint32_t>(deadline - now)));
        }
    }

    [[nodiscard]] inline bool WaitReady(
        const TaskRuntimeClient& client,
        const uint32_t timeoutMilliseconds = 2000)
    {
        return WaitUntil(
            [&client] {
                return client.State() == TaskRuntimeClientState::Ready &&
                       client.ActiveEpoch() != 0 && !client.RuntimeInstanceId().empty();
            },
            timeoutMilliseconds);
    }

    [[nodiscard]] inline bool WaitReady(
        const TaskRuntimeClient& client,
        const FakeRuntimeClientConnectionState& connection,
        const uint32_t timeoutMilliseconds = 2000)
    {
        const auto wait = WaitForSingleObject(connection.ReceiveEnteredEvent(), timeoutMilliseconds);
        return wait == WAIT_OBJECT_0 && client.State() == TaskRuntimeClientState::Ready &&
               client.ActiveEpoch() != 0;
    }

    [[nodiscard]] inline bool WaitForOutboundRequestCount(
        const FakeRuntimeClientConnectionState& connection,
        const size_t count,
        const uint32_t timeoutMilliseconds = 2000)
    {
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        while (connection.OutboundRequests().size() < count)
        {
            const auto now = GetTickCount64();
            if (now >= deadline)
            {
                return false;
            }
            if (WaitForSingleObject(
                    connection.SendEnteredEvent(),
                    static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline std::optional<RuntimeClientSubscriptionEvent> PullSubscriptionEvent(
        const std::shared_ptr<RuntimeClientSubscriptionStream>& stream,
        const uint32_t timeoutMilliseconds = 2000)
    {
        if (!stream)
        {
            return std::nullopt;
        }
        auto pulled = stream->Pull(timeoutMilliseconds);
        return pulled.Status == RuntimeClientSubscriptionPullStatus::Event ?
                   std::move(pulled.Value) :
                   std::nullopt;
    }

    [[nodiscard]] inline bool StopAndWait(TaskRuntimeClient& client) noexcept
    {
        client.RequestStop();
        return client.WaitForStopped() && client.State() == TaskRuntimeClientState::Stopped;
    }

    [[nodiscard]] inline bool WaitReady(
        const RuntimeClientRequestHandle& request,
        const uint32_t timeoutMilliseconds = 2000)
    {
        return request.Completion.valid() &&
               request.Completion.wait_for(std::chrono::milliseconds{ timeoutMilliseconds }) ==
                   std::future_status::ready;
    }

    [[nodiscard]] inline bool CancelPrecedesClose(
        const FakeRuntimeClientConnectionState& connection)
    {
        const auto lifecycle = connection.Lifecycle();
        const auto cancel = std::ranges::find(
            lifecycle,
            FakeConnectionLifecycleAction::CancelPendingIo);
        const auto close = std::ranges::find(
            lifecycle,
            FakeConnectionLifecycleAction::Close);
        return cancel != lifecycle.end() && close != lifecycle.end() && cancel < close &&
               !connection.CloseObservedWhileIoActive();
    }
}
