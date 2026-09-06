// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolCapabilities.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr size_t MaximumRpcErrorMessageBytes{ 4096 };
    inline constexpr size_t MaximumNotificationKindBytes{ 64 };

    enum class ControlMethod : uint8_t
    {
        Ping,
        SnapshotAndSubscribe,
        Unsubscribe,
        QueryTask,
        SubmitCommand,
        AcquireInputLease,
        ReleaseInputLease,
        RuntimeStatus,
        DrainRuntime,
        QueryAgents,
        AgentCommand,
        Count,
    };

    enum class ControlNotificationKind : uint8_t
    {
        ProjectionChanged,
        SubscriptionClosed,
        RuntimeHealthChanged,
        InputLeaseChanged,
    };

    enum class ProtocolRpcErrorCode : uint8_t
    {
        InvalidRequest,
        HandshakeRequired,
        ProtocolMismatch,
        CapabilityRequired,
        DuplicateRequest,
        TooManyRequests,
        NotFound,
        Conflict,
        RuntimeUnavailable,
        InternalError,
    };

    struct ControlRequest
    {
        std::string RequestId;
        ControlMethod Method{ ControlMethod::Ping };
        std::string PayloadJson{ "{}" };
    };

    struct ProtocolRpcError
    {
        ProtocolRpcErrorCode Code{ ProtocolRpcErrorCode::InternalError };
        std::string Message;
        std::string DataJson{ "{}" };
    };

    struct ControlResponse
    {
        std::string RequestId;
        bool Succeeded{};
        std::string ResultJson{ "{}" };
        std::optional<ProtocolRpcError> Error;
    };

    struct ControlNotification
    {
        std::string SubscriptionId;
        uint64_t Generation{};
        uint64_t Sequence{};
        ControlNotificationKind Kind{ ControlNotificationKind::ProjectionChanged };
        std::string UnknownKind;
        std::string PayloadJson{ "{}" };
    };

    [[nodiscard]] std::string_view ControlMethodName(ControlMethod method) noexcept;
    [[nodiscard]] std::optional<ControlMethod> ControlMethodFromName(std::string_view name) noexcept;
    [[nodiscard]] ProtocolCapabilitySet RequiredCapabilities(ControlMethod method) noexcept;
    [[nodiscard]] std::string_view ControlNotificationKindName(ControlNotificationKind kind) noexcept;
    [[nodiscard]] std::optional<ControlNotificationKind> ControlNotificationKindFromName(std::string_view name) noexcept;
    [[nodiscard]] std::string_view ProtocolRpcErrorCodeName(ProtocolRpcErrorCode code) noexcept;
    [[nodiscard]] std::optional<ProtocolRpcErrorCode> ProtocolRpcErrorCodeFromName(std::string_view name) noexcept;
}
