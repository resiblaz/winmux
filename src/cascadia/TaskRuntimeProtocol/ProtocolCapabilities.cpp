// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ProtocolCapabilities.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        constexpr std::array<std::pair<ProtocolCapability, std::string_view>, 11> CapabilityMap{
            std::pair{ ProtocolCapability::TaskQueries, "task_queries" },
            std::pair{ ProtocolCapability::TaskCommands, "task_commands" },
            std::pair{ ProtocolCapability::SnapshotAndSubscribe, "snapshot_and_subscribe" },
            std::pair{ ProtocolCapability::EventCatchUp, "event_catch_up" },
            std::pair{ ProtocolCapability::TerminalStream, "terminal_stream" },
            std::pair{ ProtocolCapability::InputControllerLease, "input_controller_lease" },
            std::pair{ ProtocolCapability::RuntimeLifecycle, "runtime_lifecycle" },
            std::pair{ ProtocolCapability::TaskTerminalSurfaces, "task_terminal_surfaces" },
            std::pair{ ProtocolCapability::TaskOrchestration, "task_orchestration" },
            std::pair{ ProtocolCapability::RuntimeDrain, "runtime_drain" },
            std::pair{ ProtocolCapability::AgentProfiles, "agent_profiles" },
        };
    }

    bool HasCapability(const ProtocolCapabilitySet capabilities, const ProtocolCapability capability) noexcept
    {
        return (capabilities & static_cast<uint64_t>(capability)) != 0;
    }

    ProtocolCapabilitySet AddCapability(const ProtocolCapabilitySet capabilities, const ProtocolCapability capability) noexcept
    {
        return capabilities | static_cast<uint64_t>(capability);
    }

    std::optional<ProtocolCapability> CapabilityFromName(const std::string_view name) noexcept
    {
        const auto match = std::ranges::find_if(CapabilityMap, [&](const auto& entry) {
            return entry.second == name;
        });
        return match == CapabilityMap.end() ? std::nullopt : std::optional<ProtocolCapability>{ match->first };
    }

    std::string_view CapabilityName(const ProtocolCapability capability) noexcept
    {
        const auto match = std::ranges::find_if(CapabilityMap, [&](const auto& entry) {
            return entry.first == capability;
        });
        return match == CapabilityMap.end() ? std::string_view{} : match->second;
    }

    std::vector<std::string> CapabilityNames(const ProtocolCapabilitySet capabilities)
    {
        std::vector<std::string> names;
        for (const auto& [capability, name] : CapabilityMap)
        {
            if (HasCapability(capabilities, capability))
            {
                names.emplace_back(name);
            }
        }
        return names;
    }
}
