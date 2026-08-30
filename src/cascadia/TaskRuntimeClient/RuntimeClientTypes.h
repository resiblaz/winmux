// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ClientRequestPolicy.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskRuntime
{
    using RuntimeClientEpoch = uint64_t;
    using RuntimeClientOperationId = uint64_t;
    using RuntimeClientAttempt = uint32_t;

    enum class RuntimeClientRetireCause
    {
        None,
        StopRequested,
        TransportFailure,
        ProtocolError,
        HeartbeatTimeout,
        Replaced,
        Count,
    };

    enum class RuntimeClientRequestAddStatus
    {
        Added,
        Invalid,
        CapacityExceeded,
        DuplicateRequestId,
        OperationAlreadyActive,
    };

    enum class RuntimeClientResponseMatch
    {
        Matched,
        DuplicateOrLate,
        UnknownRequest,
        StaleEpoch,
        Invalid,
    };

    enum class RuntimeClientRequestTombstoneKind
    {
        CompletedDuplicateGuard,
        RetiredEpoch,
        CancelledBeforeWrite,
    };

    struct RuntimeClientRequestAttemptInfo
    {
        RuntimeClientOperationId OperationId{};
        RuntimeClientAttempt Attempt{};
        std::string RequestId;
        Microsoft::Terminal::TaskRuntimeProtocol::ControlMethod Method{
            Microsoft::Terminal::TaskRuntimeProtocol::ControlMethod::Ping
        };
        RuntimeClientEpoch Epoch{};
        Microsoft::Terminal::TaskRuntimeProtocol::ClientRequestTransmission Transmission{
            Microsoft::Terminal::TaskRuntimeProtocol::ClientRequestTransmission::NotSent
        };
    };

    struct RuntimeClientRetiredRequest
    {
        RuntimeClientRequestAttemptInfo Attempt;
        Microsoft::Terminal::TaskRuntimeProtocol::ClientDisconnectAction Action{
            Microsoft::Terminal::TaskRuntimeProtocol::ClientDisconnectAction::OutcomeUnknown
        };
    };

    struct RuntimeClientCorrelatedResponse
    {
        RuntimeClientResponseMatch Status{ RuntimeClientResponseMatch::Invalid };
        std::optional<RuntimeClientRequestAttemptInfo> Attempt;
    };

    struct RuntimeClientRequestRegistryConfiguration
    {
        size_t MaximumActiveRequests{ 256 };
        size_t MaximumTombstones{ 1024 };
    };
}
