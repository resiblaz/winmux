// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolCapabilities.h"
#include "ProtocolVersion.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    enum class ProtocolClientKind : uint8_t
    {
        TerminalUi,
        TaskCtl,
        TestPeer,
    };

    struct HelloRequest
    {
        std::string ClientInstanceId;
        ProtocolClientKind ClientKind{ ProtocolClientKind::TerminalUi };
        std::string ExpectedRuntimeIdentity;
        ProtocolVersionRange Versions;
        ProtocolCapabilitySet RequiredCapabilities{};
        ProtocolCapabilitySet OptionalCapabilities{};
        std::vector<std::string> UnknownRequiredCapabilities;
        uint32_t MaximumAcceptedControlFrameBytes{ MaximumControlFrameBytes };
    };

    struct RuntimeHandshakeConfiguration
    {
        std::string RuntimeIdentity;
        std::string RuntimeInstanceId;
        ProtocolVersionRange Versions;
        ProtocolCapabilitySet Capabilities{};
        uint32_t MaximumControlFrameBytes{ ::Microsoft::Terminal::TaskRuntimeProtocol::MaximumControlFrameBytes };
        uint32_t MaximumTerminalFrameBytes{ ::Microsoft::Terminal::TaskRuntimeProtocol::MaximumTerminalFrameBytes };
        uint32_t HeartbeatMilliseconds{ 5000 };
    };

    struct HelloResponse
    {
        std::string RuntimeIdentity;
        std::string RuntimeInstanceId;
        uint16_t ProtocolMajor{};
        uint16_t ProtocolMinor{};
        ProtocolCapabilitySet Capabilities{};
        uint32_t MaximumControlFrameBytes{};
        uint32_t MaximumTerminalFrameBytes{};
        uint32_t HeartbeatMilliseconds{};
    };

    enum class HandshakeError
    {
        None,
        InvalidRequest,
        InvalidRuntimeConfiguration,
        RuntimeIdentityMismatch,
        ProtocolMajorMismatch,
        ProtocolMinorMismatch,
        MissingRequiredCapability,
    };

    struct HandshakeDecision
    {
        bool Accepted{};
        HandshakeError Error{ HandshakeError::InvalidRequest };
        HelloResponse Response;
        std::string Message;
    };

    [[nodiscard]] std::string_view ProtocolClientKindName(ProtocolClientKind kind) noexcept;
    [[nodiscard]] std::optional<ProtocolClientKind> ProtocolClientKindFromName(std::string_view name) noexcept;
    [[nodiscard]] HandshakeDecision NegotiateHandshake(
        const HelloRequest& request,
        const RuntimeHandshakeConfiguration& runtime);
    [[nodiscard]] HandshakeDecision ValidateHelloResponse(
        const HelloRequest& request,
        const HelloResponse& response);
}
