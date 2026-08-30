// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientSubscriptions.h"

#include "RuntimeControlTransport.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr size_t DefaultMaximumSubscriptionStreamEvents{ 512 };
    inline constexpr size_t AbsoluteMaximumSubscriptionStreamEvents{ 4096 };
    inline constexpr size_t DefaultMaximumSubscriptionStreamBytes{ 4 * 1024 * 1024 };
    inline constexpr size_t AbsoluteMaximumSubscriptionStreamBytes{ 64 * 1024 * 1024 };
    inline constexpr size_t DefaultMaximumClientSubscriptionAggregateBytes{ 32 * 1024 * 1024 };
    inline constexpr size_t AbsoluteMaximumClientSubscriptionAggregateBytes{ 512 * 1024 * 1024 };
    inline constexpr size_t DefaultMaximumSubscriptionEventBytes{ 1024 * 1024 + 8 * 1024 };
    inline constexpr size_t AbsoluteMaximumSubscriptionEventBytes{
        DefaultMaximumSubscriptionEventBytes
    };
    inline constexpr size_t MaximumSubscriptionEventDiagnosticBytes{ 4096 };

    struct RuntimeClientSubscribeOptions
    {
        std::string ParamsJson{ "{}" };
        // Zero selects the corresponding default bound.
        size_t MaximumQueuedEvents{};
        size_t MaximumQueuedBytes{};
        size_t MaximumEventBytes{};
    };

    enum class RuntimeClientSubscriptionEventKind
    {
        Snapshot,
        Notification,
        ResnapshotRequired,
        Removed,
        Faulted,
        ClientStopped,
    };

    // Events are transferred by value. Once published, the stream never shares
    // mutable event storage with either producer or consumer.
    struct RuntimeClientSubscriptionEvent
    {
        RuntimeClientSubscriptionEventKind Kind{ RuntimeClientSubscriptionEventKind::Snapshot };
        RuntimeClientLogicalSubscriptionId LogicalId{};
        RuntimeClientSubscriptionRevision Revision{};
        RuntimeClientEpoch Epoch{};
        uint64_t Cursor{};
        std::string SnapshotJson;
        std::optional<Microsoft::Terminal::TaskRuntimeProtocol::ControlNotification> Notification;
        RuntimeTransportStatus TransportStatus{ RuntimeTransportStatus::Succeeded };
        DWORD SystemError{};
        std::string Diagnostic;
    };

    inline constexpr size_t RuntimeClientSubscriptionEventOverheadBytes{
        sizeof(RuntimeClientSubscriptionEvent) + sizeof(size_t)
    };

    enum class RuntimeClientSubscriptionPullStatus
    {
        Event,
        Timeout,
        Cancelled,
        Closed,
    };

    struct RuntimeClientSubscriptionPullResult
    {
        RuntimeClientSubscriptionPullStatus Status{ RuntimeClientSubscriptionPullStatus::Closed };
        std::optional<RuntimeClientSubscriptionEvent> Value;
    };
}
