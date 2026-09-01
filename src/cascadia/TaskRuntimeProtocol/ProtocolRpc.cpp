// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ProtocolRpc.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    std::string_view ControlMethodName(const ControlMethod method) noexcept
    {
        switch (method)
        {
        case ControlMethod::Ping:
            return "ping";
        case ControlMethod::SnapshotAndSubscribe:
            return "snapshot_and_subscribe";
        case ControlMethod::Unsubscribe:
            return "unsubscribe";
        case ControlMethod::QueryTask:
            return "query_task";
        case ControlMethod::SubmitCommand:
            return "submit_command";
        case ControlMethod::AcquireInputLease:
            return "acquire_input_lease";
        case ControlMethod::ReleaseInputLease:
            return "release_input_lease";
        case ControlMethod::RuntimeStatus:
            return "runtime_status";
        case ControlMethod::DrainRuntime:
            return "drain_runtime";
        case ControlMethod::Count:
            return {};
        default:
            return {};
        }
    }

    std::optional<ControlMethod> ControlMethodFromName(const std::string_view name) noexcept
    {
        for (uint8_t value = static_cast<uint8_t>(ControlMethod::Ping);
             value < static_cast<uint8_t>(ControlMethod::Count);
             ++value)
        {
            const auto method = static_cast<ControlMethod>(value);
            if (ControlMethodName(method) == name)
            {
                return method;
            }
        }
        return std::nullopt;
    }

    ProtocolCapabilitySet RequiredCapabilities(const ControlMethod method) noexcept
    {
        switch (method)
        {
        case ControlMethod::Ping:
            return 0;
        case ControlMethod::SnapshotAndSubscribe:
        case ControlMethod::Unsubscribe:
            return static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe);
        case ControlMethod::QueryTask:
            return static_cast<uint64_t>(ProtocolCapability::TaskQueries);
        case ControlMethod::SubmitCommand:
            return static_cast<uint64_t>(ProtocolCapability::TaskCommands);
        case ControlMethod::AcquireInputLease:
        case ControlMethod::ReleaseInputLease:
            return static_cast<uint64_t>(ProtocolCapability::InputControllerLease);
        case ControlMethod::RuntimeStatus:
            return static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
        case ControlMethod::DrainRuntime:
            return static_cast<uint64_t>(ProtocolCapability::RuntimeDrain);
        case ControlMethod::Count:
            return AllProtocolCapabilities;
        default:
            return AllProtocolCapabilities;
        }
    }

    std::string_view ControlNotificationKindName(const ControlNotificationKind kind) noexcept
    {
        switch (kind)
        {
        case ControlNotificationKind::ProjectionChanged:
            return "projection_changed";
        case ControlNotificationKind::SubscriptionClosed:
            return "subscription_closed";
        case ControlNotificationKind::RuntimeHealthChanged:
            return "runtime_health_changed";
        case ControlNotificationKind::InputLeaseChanged:
            return "input_lease_changed";
        default:
            return {};
        }
    }

    std::optional<ControlNotificationKind> ControlNotificationKindFromName(const std::string_view name) noexcept
    {
        for (uint8_t value = static_cast<uint8_t>(ControlNotificationKind::ProjectionChanged);
             value <= static_cast<uint8_t>(ControlNotificationKind::InputLeaseChanged);
             ++value)
        {
            const auto kind = static_cast<ControlNotificationKind>(value);
            if (ControlNotificationKindName(kind) == name)
            {
                return kind;
            }
        }
        return std::nullopt;
    }

    std::string_view ProtocolRpcErrorCodeName(const ProtocolRpcErrorCode code) noexcept
    {
        switch (code)
        {
        case ProtocolRpcErrorCode::InvalidRequest:
            return "invalid_request";
        case ProtocolRpcErrorCode::HandshakeRequired:
            return "handshake_required";
        case ProtocolRpcErrorCode::ProtocolMismatch:
            return "protocol_mismatch";
        case ProtocolRpcErrorCode::CapabilityRequired:
            return "capability_required";
        case ProtocolRpcErrorCode::DuplicateRequest:
            return "duplicate_request";
        case ProtocolRpcErrorCode::TooManyRequests:
            return "too_many_requests";
        case ProtocolRpcErrorCode::NotFound:
            return "not_found";
        case ProtocolRpcErrorCode::Conflict:
            return "conflict";
        case ProtocolRpcErrorCode::RuntimeUnavailable:
            return "runtime_unavailable";
        case ProtocolRpcErrorCode::InternalError:
            return "internal_error";
        default:
            return {};
        }
    }

    std::optional<ProtocolRpcErrorCode> ProtocolRpcErrorCodeFromName(const std::string_view name) noexcept
    {
        for (uint8_t value = static_cast<uint8_t>(ProtocolRpcErrorCode::InvalidRequest);
             value <= static_cast<uint8_t>(ProtocolRpcErrorCode::InternalError);
             ++value)
        {
            const auto code = static_cast<ProtocolRpcErrorCode>(value);
            if (ProtocolRpcErrorCodeName(code) == name)
            {
                return code;
            }
        }
        return std::nullopt;
    }
}
