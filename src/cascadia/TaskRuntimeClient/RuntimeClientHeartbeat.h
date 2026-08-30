// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientScheduler.h"
#include "RuntimeClientTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr uint32_t MinimumRuntimeClientHeartbeatMilliseconds{ 250 };
    inline constexpr uint32_t MaximumRuntimeClientHeartbeatMilliseconds{ 60000 };
    inline constexpr std::string_view RuntimeClientHeartbeatRequestIdPrefix{ "f17e0001-" };

    enum class RuntimeClientHeartbeatPhase
    {
        Inactive,
        Monitoring,
        PingDue,
        PingInFlight,
        TimedOut,
        Failed,
    };

    enum class RuntimeClientHeartbeatAction
    {
        Wait,
        QueuePing,
        Timeout,
        Failed,
    };

    enum class RuntimeClientHeartbeatStatus
    {
        Succeeded,
        Inactive,
        Stale,
        Failed,
    };

    struct RuntimeClientHeartbeatDecision
    {
        RuntimeClientHeartbeatAction Action{ RuntimeClientHeartbeatAction::Failed };
        RuntimeClientMonotonicMilliseconds Deadline{};
    };

    struct RuntimeClientHeartbeatInboundResult
    {
        RuntimeClientHeartbeatStatus Status{ RuntimeClientHeartbeatStatus::Inactive };
        std::optional<RuntimeClientOperationId> SupersededOperation;
    };

    struct RuntimeClientHeartbeatView
    {
        RuntimeClientHeartbeatPhase Phase{ RuntimeClientHeartbeatPhase::Inactive };
        RuntimeClientEpoch Epoch{};
        uint32_t IntervalMilliseconds{};
        RuntimeClientMonotonicMilliseconds LastInbound{};
        RuntimeClientMonotonicMilliseconds PingDue{};
        RuntimeClientMonotonicMilliseconds SilenceDeadline{};
        RuntimeClientOperationId PingOperationId{};
        bool HasWriterWork{};
    };

    struct RuntimeClientHeartbeatRequestIdentity
    {
        uint32_t Namespace{};
        uint64_t Sequence{};
    };

    // Pure caller-serialized liveness policy. It performs no IO, waiting,
    // allocation, callback dispatch, or clock reads.
    class RuntimeClientHeartbeatState final
    {
    public:
        [[nodiscard]] bool Initialize(
            RuntimeClientEpoch epoch,
            RuntimeClientMonotonicMilliseconds now,
            uint32_t heartbeatMilliseconds) noexcept;
        [[nodiscard]] RuntimeClientHeartbeatDecision Decide(
            RuntimeClientMonotonicMilliseconds now) noexcept;
        [[nodiscard]] bool BeginPing(RuntimeClientOperationId operationId) noexcept;
        [[nodiscard]] RuntimeClientHeartbeatInboundResult ObserveInbound(
            RuntimeClientMonotonicMilliseconds now) noexcept;
        [[nodiscard]] RuntimeClientHeartbeatStatus AcknowledgePing(
            RuntimeClientOperationId operationId,
            RuntimeClientMonotonicMilliseconds now) noexcept;
        void Retire() noexcept;

        [[nodiscard]] bool HasWriterWork() const noexcept;
        [[nodiscard]] std::optional<RuntimeClientHeartbeatView> ActiveView() const noexcept;

    private:
        [[nodiscard]] bool _setWindow(RuntimeClientMonotonicMilliseconds now) noexcept;
        void _fail() noexcept;

        RuntimeClientHeartbeatPhase _phase{ RuntimeClientHeartbeatPhase::Inactive };
        RuntimeClientEpoch _epoch{};
        uint32_t _intervalMilliseconds{};
        RuntimeClientMonotonicMilliseconds _lastInbound{};
        RuntimeClientMonotonicMilliseconds _pingDue{};
        RuntimeClientMonotonicMilliseconds _silenceDeadline{};
        RuntimeClientOperationId _pingOperationId{};
    };

    [[nodiscard]] std::optional<uint32_t> RuntimeClientHeartbeatNamespace(
        std::string_view clientInstanceId) noexcept;
    [[nodiscard]] std::optional<std::string> MakeRuntimeClientHeartbeatRequestId(
        std::string_view clientInstanceId,
        uint64_t sequence) noexcept;
    [[nodiscard]] std::optional<std::string> MakeRuntimeClientHeartbeatRequestId(
        uint32_t requestNamespace,
        uint64_t sequence) noexcept;
    [[nodiscard]] std::optional<RuntimeClientHeartbeatRequestIdentity>
        ParseRuntimeClientHeartbeatRequestId(std::string_view requestId) noexcept;
    [[nodiscard]] bool IsReservedHeartbeatRequestId(std::string_view requestId) noexcept;
}
