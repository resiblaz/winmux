// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ProtocolHandshake.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        bool _validVersionRange(const ProtocolVersionRange& versions) noexcept
        {
            return versions.Major != 0 && versions.MinimumMinor <= versions.PreferredMinor;
        }

        bool _validFrameLimit(const uint32_t value) noexcept
        {
            return value >= MinimumControlFrameBytes && value <= MaximumControlFrameBytes;
        }

        bool _validUnknownRequiredCapabilities(const std::vector<std::string>& capabilities) noexcept
        {
            if (capabilities.size() > 64)
            {
                return false;
            }
            return std::ranges::all_of(capabilities, [](const auto& capability) {
                return !capability.empty() && capability.size() <= 64;
            });
        }

        HandshakeDecision _reject(const HandshakeError error, std::string message)
        {
            HandshakeDecision decision;
            decision.Error = error;
            decision.Message = std::move(message);
            return decision;
        }
    }

    std::string_view ProtocolClientKindName(const ProtocolClientKind kind) noexcept
    {
        switch (kind)
        {
        case ProtocolClientKind::TerminalUi:
            return "terminal_ui";
        case ProtocolClientKind::TaskCtl:
            return "task_ctl";
        case ProtocolClientKind::TestPeer:
            return "test_peer";
        default:
            return {};
        }
    }

    std::optional<ProtocolClientKind> ProtocolClientKindFromName(const std::string_view name) noexcept
    {
        if (name == "terminal_ui")
        {
            return ProtocolClientKind::TerminalUi;
        }
        if (name == "task_ctl")
        {
            return ProtocolClientKind::TaskCtl;
        }
        if (name == "test_peer")
        {
            return ProtocolClientKind::TestPeer;
        }
        return std::nullopt;
    }

    HandshakeDecision NegotiateHandshake(
        const HelloRequest& request,
        const RuntimeHandshakeConfiguration& runtime)
    {
        if (!IsCanonicalProtocolUuid(request.ClientInstanceId) ||
            ProtocolClientKindName(request.ClientKind).empty() ||
            !IsValidRuntimeIdentity(request.ExpectedRuntimeIdentity) ||
            !_validVersionRange(request.Versions) ||
            !_validFrameLimit(request.MaximumAcceptedControlFrameBytes) ||
            (request.RequiredCapabilities & request.OptionalCapabilities) != 0 ||
            ((request.RequiredCapabilities | request.OptionalCapabilities) & ~AllProtocolCapabilities) != 0 ||
            !_validUnknownRequiredCapabilities(request.UnknownRequiredCapabilities))
        {
            return _reject(HandshakeError::InvalidRequest, "Client handshake fields are invalid");
        }
        if (!IsValidRuntimeIdentity(runtime.RuntimeIdentity) ||
            !IsCanonicalProtocolUuid(runtime.RuntimeInstanceId) ||
            !_validVersionRange(runtime.Versions) ||
            !_validFrameLimit(runtime.MaximumControlFrameBytes) ||
            runtime.MaximumTerminalFrameBytes == 0 ||
            runtime.MaximumTerminalFrameBytes > MaximumTerminalFrameBytes ||
            runtime.HeartbeatMilliseconds < 250 || runtime.HeartbeatMilliseconds > 60000 ||
            (runtime.Capabilities & ~AllProtocolCapabilities) != 0)
        {
            return _reject(HandshakeError::InvalidRuntimeConfiguration, "Runtime handshake configuration is invalid");
        }
        if (request.ExpectedRuntimeIdentity != runtime.RuntimeIdentity)
        {
            return _reject(HandshakeError::RuntimeIdentityMismatch, "Runtime identity does not match the requested channel");
        }
        if (request.Versions.Major != runtime.Versions.Major)
        {
            return _reject(HandshakeError::ProtocolMajorMismatch, "Protocol major versions do not match");
        }

        const auto negotiatedMinor = std::min(request.Versions.PreferredMinor, runtime.Versions.PreferredMinor);
        if (negotiatedMinor < request.Versions.MinimumMinor ||
            negotiatedMinor < runtime.Versions.MinimumMinor)
        {
            return _reject(HandshakeError::ProtocolMinorMismatch, "Protocol minor version ranges do not overlap");
        }
        auto capabilities = runtime.Capabilities;
        // Agent-aware Task projections add fields to an otherwise strict schema.
        // Keep the old shape for clients that did not opt in to this extension.
        if (negotiatedMinor < 3 || !HasCapability(request.RequiredCapabilities | request.OptionalCapabilities, ProtocolCapability::AgentProfiles))
            capabilities &= ~static_cast<uint64_t>(ProtocolCapability::AgentProfiles);
        if (!request.UnknownRequiredCapabilities.empty() ||
            (request.RequiredCapabilities & capabilities) != request.RequiredCapabilities)
        {
            std::string message{ "Runtime is missing a required capability" };
            if (!request.UnknownRequiredCapabilities.empty())
            {
                message.append(": ");
                message.append(request.UnknownRequiredCapabilities.front());
            }
            return _reject(HandshakeError::MissingRequiredCapability, std::move(message));
        }

        HandshakeDecision decision;
        decision.Accepted = true;
        decision.Error = HandshakeError::None;
        decision.Response.RuntimeIdentity = runtime.RuntimeIdentity;
        decision.Response.RuntimeInstanceId = runtime.RuntimeInstanceId;
        decision.Response.ProtocolMajor = runtime.Versions.Major;
        decision.Response.ProtocolMinor = negotiatedMinor;
        decision.Response.Capabilities = capabilities;
        decision.Response.MaximumControlFrameBytes = std::min(
            request.MaximumAcceptedControlFrameBytes,
            runtime.MaximumControlFrameBytes);
        decision.Response.MaximumTerminalFrameBytes = runtime.MaximumTerminalFrameBytes;
        decision.Response.HeartbeatMilliseconds = runtime.HeartbeatMilliseconds;
        return decision;
    }

    HandshakeDecision ValidateHelloResponse(
        const HelloRequest& request,
        const HelloResponse& response)
    {
        if (!IsCanonicalProtocolUuid(request.ClientInstanceId) ||
            ProtocolClientKindName(request.ClientKind).empty() ||
            !IsValidRuntimeIdentity(request.ExpectedRuntimeIdentity) ||
            !_validVersionRange(request.Versions) ||
            !_validFrameLimit(request.MaximumAcceptedControlFrameBytes) ||
            (request.RequiredCapabilities & request.OptionalCapabilities) != 0 ||
            ((request.RequiredCapabilities | request.OptionalCapabilities) & ~AllProtocolCapabilities) != 0 ||
            !_validUnknownRequiredCapabilities(request.UnknownRequiredCapabilities))
        {
            return _reject(HandshakeError::InvalidRequest, "Client handshake fields are invalid");
        }
        if (!IsValidRuntimeIdentity(response.RuntimeIdentity) ||
            !IsCanonicalProtocolUuid(response.RuntimeInstanceId) ||
            response.ProtocolMajor == 0 ||
            (response.Capabilities & ~AllProtocolCapabilities) != 0 ||
            !_validFrameLimit(response.MaximumControlFrameBytes) ||
            response.MaximumControlFrameBytes > request.MaximumAcceptedControlFrameBytes ||
            response.MaximumTerminalFrameBytes == 0 ||
            response.MaximumTerminalFrameBytes > MaximumTerminalFrameBytes ||
            response.HeartbeatMilliseconds < 250 || response.HeartbeatMilliseconds > 60000)
        {
            return _reject(HandshakeError::InvalidRuntimeConfiguration, "Runtime Hello response is invalid");
        }
        if (response.RuntimeIdentity != request.ExpectedRuntimeIdentity)
        {
            return _reject(HandshakeError::RuntimeIdentityMismatch, "Runtime identity does not match the requested channel");
        }
        if (response.ProtocolMajor != request.Versions.Major)
        {
            return _reject(HandshakeError::ProtocolMajorMismatch, "Protocol major versions do not match");
        }
        if (response.ProtocolMinor < request.Versions.MinimumMinor ||
            response.ProtocolMinor > request.Versions.PreferredMinor)
        {
            return _reject(HandshakeError::ProtocolMinorMismatch, "Runtime selected a minor version outside the client range");
        }
        if (!request.UnknownRequiredCapabilities.empty() ||
            (request.RequiredCapabilities & response.Capabilities) != request.RequiredCapabilities)
        {
            return _reject(HandshakeError::MissingRequiredCapability, "Runtime response omits a required capability");
        }

        HandshakeDecision decision;
        decision.Accepted = true;
        decision.Error = HandshakeError::None;
        decision.Response = response;
        return decision;
    }
}
