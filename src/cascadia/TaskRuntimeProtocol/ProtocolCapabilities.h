// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    enum class ProtocolCapability : uint64_t
    {
        TaskQueries = 1ull << 0,
        TaskCommands = 1ull << 1,
        SnapshotAndSubscribe = 1ull << 2,
        EventCatchUp = 1ull << 3,
        TerminalStream = 1ull << 4,
        InputControllerLease = 1ull << 5,
        RuntimeLifecycle = 1ull << 6,
    };

    using ProtocolCapabilitySet = uint64_t;

    inline constexpr ProtocolCapabilitySet AllProtocolCapabilities{
        static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
        static_cast<uint64_t>(ProtocolCapability::TaskCommands) |
        static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe) |
        static_cast<uint64_t>(ProtocolCapability::EventCatchUp) |
        static_cast<uint64_t>(ProtocolCapability::TerminalStream) |
        static_cast<uint64_t>(ProtocolCapability::InputControllerLease) |
        static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle)
    };

    [[nodiscard]] bool HasCapability(ProtocolCapabilitySet capabilities, ProtocolCapability capability) noexcept;
    [[nodiscard]] ProtocolCapabilitySet AddCapability(ProtocolCapabilitySet capabilities, ProtocolCapability capability) noexcept;
    [[nodiscard]] std::optional<ProtocolCapability> CapabilityFromName(std::string_view name) noexcept;
    [[nodiscard]] std::string_view CapabilityName(ProtocolCapability capability) noexcept;
    [[nodiscard]] std::vector<std::string> CapabilityNames(ProtocolCapabilitySet capabilities);
}
