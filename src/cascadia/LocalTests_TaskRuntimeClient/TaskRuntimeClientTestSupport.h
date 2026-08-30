// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "FakeRuntimeClientConnector.h"
#include "FakeRuntimeClientServices.h"

#include "TaskRuntimeClient.h"

#include "ProtocolCapabilities.h"
#include "ProtocolVersion.h"

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
        configuration.MaximumTombstones = 128;
        configuration.MaximumQueuedBytes = 1024 * 1024;
        configuration.MaximumRequestBytes = 256 * 1024;
        configuration.MaximumRememberedRequestIds = 256;
        configuration.DefaultMaximumAttempts = 3;
        configuration.MaximumAttemptsPerOperation = 8;
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
        response.HeartbeatMilliseconds = 1000;
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
