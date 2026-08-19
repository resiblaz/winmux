// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceListeningPorts.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "iphlpapi.lib")

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr size_t MaximumProcessTreeDepth{ 128 };

        uint16_t _hostPort(const DWORD networkPort) noexcept
        {
            const auto value = static_cast<uint16_t>(networkPort);
            return static_cast<uint16_t>((value >> 8) | (value << 8));
        }

        bool _queryProcesses(std::vector<WorkspaceListeningPorts::ProcessRecord>& processes)
        {
            const auto rawSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (rawSnapshot == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            const wil::unique_handle snapshot{ rawSnapshot };
            PROCESSENTRY32W entry{ sizeof(entry) };
            if (!Process32FirstW(snapshot.get(), &entry))
            {
                return false;
            }

            do
            {
                processes.emplace_back(entry.th32ProcessID, entry.th32ParentProcessID);
            } while (Process32NextW(snapshot.get(), &entry));

            return GetLastError() == ERROR_NO_MORE_FILES;
        }

        template<typename Table, typename RowCallback>
        bool _queryTcpTable(const ULONG addressFamily, RowCallback&& addRow)
        {
            DWORD size{};
            auto result = GetExtendedTcpTable(nullptr,
                                              &size,
                                              FALSE,
                                              addressFamily,
                                              TCP_TABLE_OWNER_PID_LISTENER,
                                              0);
            if (result != ERROR_INSUFFICIENT_BUFFER && result != NO_ERROR)
            {
                return false;
            }
            if (size == 0)
            {
                return result == NO_ERROR;
            }

            std::vector<std::byte> buffer;
            for (size_t attempt = 0; attempt < 3; ++attempt)
            {
                buffer.resize(size);
                result = GetExtendedTcpTable(buffer.data(),
                                             &size,
                                             FALSE,
                                             addressFamily,
                                             TCP_TABLE_OWNER_PID_LISTENER,
                                             0);
                if (result == ERROR_INSUFFICIENT_BUFFER)
                {
                    continue;
                }
                if (result != NO_ERROR)
                {
                    return false;
                }

                const auto table = reinterpret_cast<const Table*>(buffer.data());
                for (DWORD index = 0; index < table->dwNumEntries; ++index)
                {
                    addRow(table->table[index]);
                }
                return true;
            }
            return false;
        }

        bool _queryListeners(std::vector<WorkspaceListeningPorts::ListenerRecord>& listeners)
        {
            const auto addRow = [&](const auto& row) {
                const auto port = _hostPort(row.dwLocalPort);
                if (row.dwOwningPid != 0 && port != 0)
                {
                    listeners.emplace_back(row.dwOwningPid, port);
                }
            };

            const auto ipv4Succeeded = _queryTcpTable<MIB_TCPTABLE_OWNER_PID>(AF_INET, addRow);
            const auto ipv6Succeeded = _queryTcpTable<MIB_TCP6TABLE_OWNER_PID>(AF_INET6, addRow);
            return ipv4Succeeded && ipv6Succeeded;
        }
    }

    WorkspaceListeningPorts::PortsByRootProcess WorkspaceListeningPorts::MapListenersToRoots(
        const std::span<const uint32_t> rootProcessIds,
        const std::span<const ProcessRecord> processes,
        const std::span<const ListenerRecord> listeners)
    {
        std::unordered_set<uint32_t> roots;
        roots.reserve(rootProcessIds.size());
        for (const auto processId : rootProcessIds)
        {
            if (processId != 0)
            {
                roots.emplace(processId);
            }
        }

        std::unordered_map<uint32_t, uint32_t> parents;
        parents.reserve(processes.size());
        for (const auto& process : processes)
        {
            if (process.ProcessId != 0)
            {
                parents.insert_or_assign(process.ProcessId, process.ParentProcessId);
            }
        }

        PortsByRootProcess result;
        for (const auto& listener : listeners)
        {
            if (listener.ProcessId == 0 || listener.Port == 0)
            {
                continue;
            }

            auto processId = listener.ProcessId;
            for (size_t depth = 0; processId != 0 && depth < MaximumProcessTreeDepth; ++depth)
            {
                if (roots.contains(processId))
                {
                    result[processId].emplace_back(listener.Port);
                    break;
                }

                const auto parent = parents.find(processId);
                if (parent == parents.end() || parent->second == processId)
                {
                    break;
                }
                processId = parent->second;
            }
        }

        for (auto& [processId, ports] : result)
        {
            std::sort(ports.begin(), ports.end());
            ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
        }
        return result;
    }

    WorkspaceListeningPorts::Snapshot WorkspaceListeningPorts::Query(const std::span<const uint32_t> rootProcessIds) noexcept
    try
    {
        if (rootProcessIds.empty())
        {
            return { true, {} };
        }

        std::vector<ProcessRecord> processes;
        if (!_queryProcesses(processes))
        {
            return {};
        }

        std::vector<ListenerRecord> listeners;
        if (!_queryListeners(listeners))
        {
            return {};
        }

        return { true, MapListenersToRoots(rootProcessIds, processes, listeners) };
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return {};
    }
}
