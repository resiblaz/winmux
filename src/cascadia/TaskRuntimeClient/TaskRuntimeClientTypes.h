// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientTypes.h"

#include "RuntimeControlTransport.h"
#include "RuntimeIdentity.h"

#include "ProtocolHandshake.h"
#include "ProtocolRpc.h"

#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class TaskRuntimeClientState
    {
        Stopped,
        Connecting,
        Ready,
        Backoff,
        Faulted,
        Stopping,
    };

    enum class RuntimeClientRequestOutcome
    {
        Succeeded,
        RpcError,
        Disconnected,
        OutcomeUnknown,
        ResnapshotRequired,
        Cancelled,
        ClientStopped,
        ProtocolError,
        CapacityExceeded,
        InvalidRequest,
    };

    struct TaskRuntimeClientConfiguration
    {
        RuntimeIdentity Identity;
        Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest Hello;
        uint32_t ConnectTimeoutMilliseconds{ 5000 };
        uint32_t SendTimeoutMilliseconds{ 5000 };
        uint32_t ReceiveTimeoutMilliseconds{ 1000 };
        size_t MaximumOperations{ 256 };
        size_t MaximumTombstones{ 1024 };
        size_t MaximumQueuedBytes{ 8 * 1024 * 1024 };
        size_t MaximumRequestBytes{ 1024 * 1024 };
        size_t MaximumRememberedRequestIds{ 65536 };
        uint32_t DefaultMaximumAttempts{ 3 };
        uint32_t MaximumAttemptsPerOperation{ 8 };
        size_t MaximumDiagnosticBytes{ 4096 };
    };

    struct RuntimeClientRequestResult
    {
        RuntimeClientOperationId OperationId{};
        RuntimeClientRequestOutcome Outcome{ RuntimeClientRequestOutcome::ProtocolError };
        RuntimeClientEpoch Epoch{};
        RuntimeClientAttempt Attempts{};
        std::string LastRequestId;
        std::optional<Microsoft::Terminal::TaskRuntimeProtocol::ControlResponse> Response;
        RuntimeTransportStatus TransportStatus{ RuntimeTransportStatus::Succeeded };
        DWORD SystemError{};
        std::string Diagnostic;
    };

    struct RuntimeClientRequestHandle
    {
        bool Accepted{};
        RuntimeClientOperationId OperationId{};
        std::shared_future<RuntimeClientRequestResult> Completion;
    };
}
