// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceListeningPorts.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#pragma comment(lib, "iphlpapi.lib")

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr size_t MaximumProcessTreeDepth{ 128 };
        constexpr ULONG InitialCommandLineBufferBytes{ 1024 };
        constexpr ULONG MaximumCommandLineBufferBytes{ 32768 };
        constexpr PROCESSINFOCLASS ProcessCommandLineInformationClass{ static_cast<PROCESSINFOCLASS>(60) };
        constexpr NTSTATUS StatusBufferTooSmall{ static_cast<NTSTATUS>(0xC0000023L) };
        constexpr NTSTATUS StatusInfoLengthMismatch{ static_cast<NTSTATUS>(0xC0000004L) };
        constexpr NTSTATUS StatusBufferOverflow{ static_cast<NTSTATUS>(0x80000005L) };

        std::wstring _toLowerAscii(const std::wstring_view text)
        {
            std::wstring lowered{ text };
            for (auto& ch : lowered)
            {
                if (ch >= L'A' && ch <= L'Z')
                {
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
            }
            return lowered;
        }

        std::wstring_view _fileNameOnly(const std::wstring_view path) noexcept
        {
            const auto pos = path.find_last_of(L"\\/");
            return pos == std::wstring_view::npos ? path : path.substr(pos + 1);
        }

        std::wstring_view _stripKnownExtension(const std::wstring_view fileName) noexcept
        {
            const auto dot = fileName.rfind(L'.');
            if (dot == std::wstring_view::npos || dot == 0)
            {
                return fileName;
            }
            const auto ext = fileName.substr(dot);
            auto equalsIgnoreCase = [](const std::wstring_view left, const std::wstring_view right) noexcept {
                if (left.size() != right.size())
                {
                    return false;
                }
                for (size_t i = 0; i < left.size(); ++i)
                {
                    auto a = left[i];
                    auto b = right[i];
                    if (a >= L'A' && a <= L'Z')
                    {
                        a = static_cast<wchar_t>(a - L'A' + L'a');
                    }
                    if (b >= L'A' && b <= L'Z')
                    {
                        b = static_cast<wchar_t>(b - L'A' + L'a');
                    }
                    if (a != b)
                    {
                        return false;
                    }
                }
                return true;
            };

            if (equalsIgnoreCase(ext, L".exe") ||
                equalsIgnoreCase(ext, L".cmd") ||
                equalsIgnoreCase(ext, L".bat") ||
                equalsIgnoreCase(ext, L".ps1") ||
                equalsIgnoreCase(ext, L".js") ||
                equalsIgnoreCase(ext, L".mjs") ||
                equalsIgnoreCase(ext, L".cjs") ||
                equalsIgnoreCase(ext, L".py"))
            {
                return fileName.substr(0, dot);
            }
            return fileName;
        }

        std::wstring _cleanDisplayName(const std::wstring_view exeName)
        {
            const auto fileName = _fileNameOnly(exeName);
            const auto stripped = _stripKnownExtension(fileName);
            return std::wstring{ stripped.empty() ? fileName : stripped };
        }

        std::wstring _lowerBaseName(const std::wstring_view exeName)
        {
            return _toLowerAscii(_stripKnownExtension(_fileNameOnly(exeName)));
        }

        bool _isInfrastructureProcess(const std::wstring_view lowerBase) noexcept
        {
            return lowerBase == L"conhost" ||
                   lowerBase == L"openconsole" ||
                   lowerBase == L"wslhost" ||
                   lowerBase == L"wslrelay" ||
                   lowerBase == L"ssh-agent" ||
                   lowerBase == L"gpg-agent" ||
                   lowerBase == L"csrss" ||
                   lowerBase == L"runtimebroker" ||
                   lowerBase == L"dllhost" ||
                   lowerBase == L"svchost" ||
                   lowerBase == L"sihost" ||
                   lowerBase == L"fontdrvhost";
        }

        bool _isWslShell(const std::wstring_view lowerBase) noexcept
        {
            return lowerBase == L"wsl" ||
                   lowerBase == L"ubuntu" ||
                   lowerBase == L"debian" ||
                   lowerBase == L"kali" ||
                   lowerBase == L"arch" ||
                   lowerBase == L"alpine";
        }

        bool _isInteractiveShell(const std::wstring_view lowerBase) noexcept
        {
            return lowerBase == L"pwsh" ||
                   lowerBase == L"powershell" ||
                   lowerBase == L"cmd" ||
                   lowerBase == L"bash" ||
                   lowerBase == L"sh" ||
                   lowerBase == L"zsh" ||
                   lowerBase == L"fish" ||
                   lowerBase == L"nu" ||
                   lowerBase == L"git-bash" ||
                   lowerBase == L"mintty" ||
                   _isWslShell(lowerBase);
        }

        bool _isWrapperProcess(const std::wstring_view lowerBase) noexcept
        {
            return lowerBase == L"node" ||
                   lowerBase == L"bun" ||
                   lowerBase == L"deno" ||
                   lowerBase == L"python" ||
                   lowerBase == L"python3" ||
                   lowerBase == L"py" ||
                   lowerBase == L"cmd" ||
                   lowerBase == L"pwsh" ||
                   lowerBase == L"powershell" ||
                   lowerBase == L"bash" ||
                   lowerBase == L"sh" ||
                   _isWslShell(lowerBase);
        }

        std::vector<std::wstring> _splitCommandLineTokens(const std::wstring_view commandLine)
        {
            std::vector<std::wstring> tokens;
            size_t i = 0;
            const auto length = commandLine.size();
            while (i < length)
            {
                while (i < length && (commandLine[i] == L' ' || commandLine[i] == L'\t' || commandLine[i] == L'\r' || commandLine[i] == L'\n'))
                {
                    ++i;
                }
                if (i >= length)
                {
                    break;
                }

                std::wstring token;
                bool inQuotes = false;
                while (i < length)
                {
                    const auto ch = commandLine[i];
                    if (ch == L'"')
                    {
                        inQuotes = !inQuotes;
                        ++i;
                        continue;
                    }
                    if (!inQuotes && (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'))
                    {
                        break;
                    }
                    token.push_back(ch);
                    ++i;
                }
                if (!token.empty())
                {
                    tokens.emplace_back(std::move(token));
                }
            }
            return tokens;
        }

        std::wstring_view _matchAgentBaseName(const std::wstring_view lowerBase) noexcept
        {
            if (lowerBase == L"claude" || lowerBase == L"claude-code")
            {
                return L"claude";
            }
            if (lowerBase == L"codex" || lowerBase == L"codex-cli")
            {
                return L"codex";
            }
            if (lowerBase == L"agy" || lowerBase == L"antigravity" || lowerBase == L"antigravity-cli")
            {
                return L"agy";
            }
            return {};
        }

        std::wstring_view _matchAgentToken(const std::wstring_view rawToken)
        {
            if (rawToken.empty() || rawToken.front() == L'-' || rawToken.front() == L'/')
            {
                return {};
            }

            const auto lowered = _toLowerAscii(rawToken);
            if (lowered.find(L"@anthropic-ai\\claude-code") != std::wstring::npos ||
                lowered.find(L"@anthropic-ai/claude-code") != std::wstring::npos)
            {
                return L"claude";
            }
            if (lowered.find(L"@openai\\codex") != std::wstring::npos ||
                lowered.find(L"@openai/codex") != std::wstring::npos)
            {
                return L"codex";
            }
            if (lowered.find(L"antigravity-cli") != std::wstring::npos)
            {
                return L"agy";
            }

            // If this argument is a quoted subcommand (e.g. cmd /c "claude --resume"),
            // inspect its leading executable/script token as well.
            std::wstring_view target{ lowered };
            if (const auto firstSpace = target.find_first_of(L" \t"); firstSpace != std::wstring_view::npos)
            {
                target = target.substr(0, firstSpace);
            }
            while (!target.empty() && (target.front() == L'"' || target.front() == L'\'' || target.front() == L'&'))
            {
                target.remove_prefix(1);
            }
            while (!target.empty() && (target.back() == L'"' || target.back() == L'\''))
            {
                target.remove_suffix(1);
            }

            return _matchAgentBaseName(_stripKnownExtension(_fileNameOnly(target)));
        }

        std::wstring_view _detectAgentLabel(const std::wstring_view exeName, const std::wstring_view commandLine)
        {
            if (const auto fromExe = _matchAgentBaseName(_lowerBaseName(exeName)); !fromExe.empty())
            {
                return fromExe;
            }
            if (commandLine.empty())
            {
                return {};
            }

            const auto tokens = _splitCommandLineTokens(commandLine);
            for (size_t index = 1; index < tokens.size(); ++index)
            {
                if (const auto matched = _matchAgentToken(tokens[index]); !matched.empty())
                {
                    return matched;
                }
            }
            return {};
        }

        std::wstring _queryProcessCommandLine(const uint32_t processId) noexcept
        try
        {
            if (processId == 0)
            {
                return {};
            }

            const wil::unique_handle processHandle{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId) };
            if (!processHandle)
            {
                return {};
            }

            using PfnNtQueryInformationProcess = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
            static const auto pfnQuery = []() noexcept -> PfnNtQueryInformationProcess {
                if (const auto ntdll = GetModuleHandleW(L"ntdll.dll"))
                {
                    return reinterpret_cast<PfnNtQueryInformationProcess>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
                }
                return nullptr;
            }();
            if (!pfnQuery)
            {
                return {};
            }

            ULONG bufferSize = InitialCommandLineBufferBytes;
            std::vector<std::byte> buffer;
            for (size_t attempt = 0; attempt < 2; ++attempt)
            {
                if (bufferSize > MaximumCommandLineBufferBytes)
                {
                    return {};
                }
                buffer.resize(bufferSize);
                ULONG returnLength = 0;
                const auto status = pfnQuery(processHandle.get(),
                                             ProcessCommandLineInformationClass,
                                             buffer.data(),
                                             bufferSize,
                                             &returnLength);
                if (status == StatusBufferTooSmall || status == StatusInfoLengthMismatch || status == StatusBufferOverflow)
                {
                    if (returnLength <= bufferSize || returnLength > MaximumCommandLineBufferBytes)
                    {
                        return {};
                    }
                    bufferSize = returnLength;
                    continue;
                }
                if (!NT_SUCCESS(status) || returnLength < sizeof(UNICODE_STRING))
                {
                    return {};
                }

                const auto* unicodeString = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
                if (!unicodeString->Buffer || unicodeString->Length == 0)
                {
                    return {};
                }

                const auto bufferBegin = reinterpret_cast<uintptr_t>(buffer.data());
                const auto bufferEnd = bufferBegin + buffer.size();
                const auto textBegin = reinterpret_cast<uintptr_t>(unicodeString->Buffer);
                const auto textBytes = static_cast<size_t>(unicodeString->Length);
                if (textBegin < bufferBegin || textBegin > bufferEnd || textBytes > (bufferEnd - textBegin))
                {
                    return {};
                }

                return std::wstring{ unicodeString->Buffer, textBytes / sizeof(wchar_t) };
            }
            return {};
        }
        catch (...)
        {
            return {};
        }

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
                processes.emplace_back(entry.th32ProcessID, entry.th32ParentProcessID, std::wstring{ entry.szExeFile });
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

    WorkspaceListeningPorts::ProcessNamesByRoot WorkspaceListeningPorts::MapProcessNamesToRoots(
        const std::span<const uint32_t> rootProcessIds,
        const std::span<const ProcessRecord> processes)
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

        ProcessNamesByRoot result;
        for (const auto& process : processes)
        {
            if (process.ProcessId == 0 || process.ExecutableName.empty())
            {
                continue;
            }

            auto processId = process.ProcessId;
            for (size_t depth = 0; processId != 0 && depth < MaximumProcessTreeDepth; ++depth)
            {
                if (roots.contains(processId))
                {
                    result[processId].emplace_back(process.ExecutableName);
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
        return result;
    }

    WorkspaceListeningPorts::ProcessActivityByRoot WorkspaceListeningPorts::MapProcessActivityToRoots(
        const std::span<const uint32_t> rootProcessIds,
        const std::span<const ProcessRecord> processes)
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

        struct Candidate
        {
            int Tier{ 0 };
            size_t Depth{ 0 };
            bool NonCmdWrapper{ false };
            std::wstring Name;
            std::wstring CommandLine;
        };

        ProcessActivityByRoot result;
        std::unordered_map<uint32_t, Candidate> bestCandidates;

        for (const auto& process : processes)
        {
            if (process.ProcessId == 0 || process.ExecutableName.empty())
            {
                continue;
            }

            uint32_t matchedRoot = 0;
            size_t matchedDepth = 0;
            auto currentId = process.ProcessId;
            for (size_t depth = 0; currentId != 0 && depth < MaximumProcessTreeDepth; ++depth)
            {
                if (roots.contains(currentId))
                {
                    matchedRoot = currentId;
                    matchedDepth = depth;
                    break;
                }

                const auto parent = parents.find(currentId);
                if (parent == parents.end() || parent->second == currentId)
                {
                    break;
                }
                currentId = parent->second;
            }

            if (matchedRoot == 0)
            {
                continue;
            }

            auto& activity = result[matchedRoot];
            const auto lowerBase = _lowerBaseName(process.ExecutableName);

            if (matchedDepth == 0)
            {
                if (_isWslShell(lowerBase))
                {
                    activity.AllowsTitleFallbackWhenIdle = true;
                }
                else if (!_isInteractiveShell(lowerBase))
                {
                    activity.HasActiveChild = true;
                }
            }
            else
            {
                if (_isInfrastructureProcess(lowerBase))
                {
                    continue;
                }
                activity.HasActiveChild = true;
            }

            Candidate candidate{};
            candidate.Depth = matchedDepth;
            candidate.CommandLine = process.CommandLine.empty() ? process.ExecutableName : process.CommandLine;

            if (const auto agent = _detectAgentLabel(process.ExecutableName, process.CommandLine); !agent.empty())
            {
                candidate.Tier = 3;
                candidate.Name = std::wstring{ agent };
                activity.HasActiveChild = true;
            }
            else if (matchedDepth > 0 || !_isInteractiveShell(lowerBase))
            {
                candidate.Tier = _isWrapperProcess(lowerBase) ? 1 : 2;
                candidate.NonCmdWrapper = (lowerBase != L"cmd");
                candidate.Name = _cleanDisplayName(process.ExecutableName);
            }
            else
            {
                continue;
            }

            auto& best = bestCandidates[matchedRoot];
            const auto better = (candidate.Tier > best.Tier) ||
                                (candidate.Tier == best.Tier && candidate.Tier == 3 && (best.Name.empty() || candidate.Depth < best.Depth)) ||
                                (candidate.Tier == best.Tier && candidate.Tier == 2 && candidate.Depth > best.Depth) ||
                                (candidate.Tier == best.Tier && candidate.Tier == 1 &&
                                 ((candidate.NonCmdWrapper && !best.NonCmdWrapper) ||
                                  (candidate.NonCmdWrapper == best.NonCmdWrapper && candidate.Depth > best.Depth)));
            if (better)
            {
                best = std::move(candidate);
            }
        }

        for (auto& [rootPid, best] : bestCandidates)
        {
            if (best.Tier > 0)
            {
                auto& activity = result[rootPid];
                activity.ActiveProcessName = std::move(best.Name);
                activity.ActiveCommandLine = std::move(best.CommandLine);
            }
        }

        return result;
    }

    WorkspaceListeningPorts::Snapshot WorkspaceListeningPorts::Query(const std::span<const uint32_t> rootProcessIds) noexcept
    try
    {
        if (rootProcessIds.empty())
        {
            return { true, {}, {}, {} };
        }

        std::vector<ProcessRecord> processes;
        if (!_queryProcesses(processes))
        {
            return {};
        }

        // Enrich command lines ONLY for wrapper processes inside our tracked terminal subtrees
        // so agent CLIs hosted on Node/Cmd/PowerShell/Python are accurately identified with minimal cost.
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
        for (auto& process : processes)
        {
            if (process.ProcessId == 0 || process.ExecutableName.empty())
            {
                continue;
            }
            const auto lowerBase = _lowerBaseName(process.ExecutableName);
            if (!_isWrapperProcess(lowerBase))
            {
                continue;
            }

            auto currentId = process.ProcessId;
            for (size_t depth = 0; currentId != 0 && depth < MaximumProcessTreeDepth; ++depth)
            {
                if (roots.contains(currentId))
                {
                    process.CommandLine = _queryProcessCommandLine(process.ProcessId);
                    break;
                }
                const auto parent = parents.find(currentId);
                if (parent == parents.end() || parent->second == currentId)
                {
                    break;
                }
                currentId = parent->second;
            }
        }

        std::vector<ListenerRecord> listeners;
        if (!_queryListeners(listeners))
        {
            return {};
        }

        return {
            true,
            MapListenersToRoots(rootProcessIds, processes, listeners),
            MapProcessNamesToRoots(rootProcessIds, processes),
            MapProcessActivityToRoots(rootProcessIds, processes),
        };
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return {};
    }
}
