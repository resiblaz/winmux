// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

namespace winrt::TerminalApp::implementation
{
    class WorkspaceListeningPorts final
    {
    public:
        struct ProcessRecord
        {
            uint32_t ProcessId{};
            uint32_t ParentProcessId{};
        };

        struct ListenerRecord
        {
            uint32_t ProcessId{};
            uint16_t Port{};
        };

        using PortsByRootProcess = std::unordered_map<uint32_t, std::vector<uint16_t>>;

        struct Snapshot
        {
            bool Succeeded{ false };
            PortsByRootProcess Ports;
        };

        static PortsByRootProcess MapListenersToRoots(std::span<const uint32_t> rootProcessIds,
                                                      std::span<const ProcessRecord> processes,
                                                      std::span<const ListenerRecord> listeners);
        static Snapshot Query(std::span<const uint32_t> rootProcessIds) noexcept;
    };
}
