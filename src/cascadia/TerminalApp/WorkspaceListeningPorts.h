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
            std::wstring ExecutableName{};
        };

        struct ListenerRecord
        {
            uint32_t ProcessId{};
            uint16_t Port{};
        };

        using PortsByRootProcess = std::unordered_map<uint32_t, std::vector<uint16_t>>;
        using ProcessNamesByRoot = std::unordered_map<uint32_t, std::vector<std::wstring>>;

        struct Snapshot
        {
            bool Succeeded{ false };
            PortsByRootProcess Ports;
            ProcessNamesByRoot ProcessNames;
        };

        static PortsByRootProcess MapListenersToRoots(std::span<const uint32_t> rootProcessIds,
                                                      std::span<const ProcessRecord> processes,
                                                      std::span<const ListenerRecord> listeners);
        static ProcessNamesByRoot MapProcessNamesToRoots(std::span<const uint32_t> rootProcessIds,
                                                         std::span<const ProcessRecord> processes);
        static Snapshot Query(std::span<const uint32_t> rootProcessIds) noexcept;
    };
}
