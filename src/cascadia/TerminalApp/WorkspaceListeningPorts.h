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
            std::wstring CommandLine{};
        };

        struct ListenerRecord
        {
            uint32_t ProcessId{};
            uint16_t Port{};
        };

        struct ProcessActivity
        {
            bool HasActiveChild{ false };
            bool AllowsTitleFallbackWhenIdle{ false };
            std::wstring ActiveProcessName{};
            std::wstring ActiveCommandLine{};

            bool operator==(const ProcessActivity&) const noexcept = default;
        };

        using PortsByRootProcess = std::unordered_map<uint32_t, std::vector<uint16_t>>;
        using ProcessNamesByRoot = std::unordered_map<uint32_t, std::vector<std::wstring>>;
        using ProcessActivityByRoot = std::unordered_map<uint32_t, ProcessActivity>;

        struct Snapshot
        {
            bool Succeeded{ false };
            PortsByRootProcess Ports;
            ProcessNamesByRoot ProcessNames;
            ProcessActivityByRoot ProcessActivities;
        };

        static PortsByRootProcess MapListenersToRoots(std::span<const uint32_t> rootProcessIds,
                                                      std::span<const ProcessRecord> processes,
                                                      std::span<const ListenerRecord> listeners);
        static ProcessNamesByRoot MapProcessNamesToRoots(std::span<const uint32_t> rootProcessIds,
                                                         std::span<const ProcessRecord> processes);
        static ProcessActivityByRoot MapProcessActivityToRoots(std::span<const uint32_t> rootProcessIds,
                                                               std::span<const ProcessRecord> processes);
        static Snapshot Query(std::span<const uint32_t> rootProcessIds) noexcept;
    };
}
