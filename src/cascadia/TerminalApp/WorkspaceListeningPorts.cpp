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

        struct CachedCommandLineEntry
        {
            uint64_t CreationTime{};
            std::wstring CommandLine;
        };

        std::mutex _commandLineCacheMutex;
        std::unordered_map<uint32_t, CachedCommandLineEntry> _commandLineCache;

        uint64_t _fileTimeToUInt64(const FILETIME& ft) noexcept
        {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
        }

        std::wstring _queryProcessCommandLineCached(const uint32_t processId) noexcept
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

            FILETIME creationTime{};
            FILETIME exitTime{};
            FILETIME kernelTime{};
            FILETIME userTime{};
            uint64_t creationStamp = 0;
            if (GetProcessTimes(processHandle.get(), &creationTime, &exitTime, &kernelTime, &userTime))
            {
                creationStamp = _fileTimeToUInt64(creationTime);
                const std::scoped_lock lock{ _commandLineCacheMutex };
                if (const auto it = _commandLineCache.find(processId); it != _commandLineCache.end() && it->second.CreationTime == creationStamp)
                {
                    return it->second.CommandLine;
                }
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
            std::wstring resolved;
            for (size_t attempt = 0; attempt < 2; ++attempt)
            {
                if (bufferSize > MaximumCommandLineBufferBytes)
                {
                    break;
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
                        break;
                    }
                    bufferSize = returnLength;
                    continue;
                }
                if (!NT_SUCCESS(status) || returnLength < sizeof(UNICODE_STRING))
                {
                    break;
                }

                const auto* unicodeString = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
                if (!unicodeString->Buffer || unicodeString->Length == 0)
                {
                    break;
                }

                const auto bufferBegin = reinterpret_cast<uintptr_t>(buffer.data());
                const auto bufferEnd = bufferBegin + buffer.size();
                const auto textBegin = reinterpret_cast<uintptr_t>(unicodeString->Buffer);
                const auto textBytes = static_cast<size_t>(unicodeString->Length);
                if (textBegin < bufferBegin || textBegin > bufferEnd || textBytes > (bufferEnd - textBegin))
                {
                    break;
                }

                resolved.assign(unicodeString->Buffer, textBytes / sizeof(wchar_t));
                break;
            }

            if (creationStamp != 0)
            {
                const std::scoped_lock lock{ _commandLineCacheMutex };
                if (_commandLineCache.size() > 512)
                {
                    _commandLineCache.clear();
                }
                _commandLineCache.insert_or_assign(processId, CachedCommandLineEntry{ creationStamp, resolved });
            }
            return resolved;
        }
        catch (...)
        {
            return {};
        }

        void _pruneCommandLineCache(const std::unordered_map<uint32_t, uint32_t>& liveParents) noexcept
        {
            const std::scoped_lock lock{ _commandLineCacheMutex };
            for (auto it = _commandLineCache.begin(); it != _commandLineCache.end();)
            {
                if (!liveParents.contains(it->first))
                {
                    it = _commandLineCache.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        struct ProcessTreeLookup
        {
            struct RootMatch
            {
                uint32_t RootPid{ 0 };
                size_t Depth{ 0 };
            };

            std::unordered_set<uint32_t> Roots;
            std::unordered_map<uint32_t, uint32_t> Parents;
            std::unordered_map<uint32_t, RootMatch> Memo;

            ProcessTreeLookup(const std::span<const uint32_t> rootProcessIds,
                              const std::span<const WorkspaceListeningPorts::ProcessRecord> processes)
            {
                Roots.reserve(rootProcessIds.size());
                for (const auto processId : rootProcessIds)
                {
                    if (processId != 0)
                    {
                        Roots.emplace(processId);
                    }
                }
                Parents.reserve(processes.size());
                Memo.reserve(processes.size());
                for (const auto& process : processes)
                {
                    if (process.ProcessId != 0)
                    {
                        Parents.insert_or_assign(process.ProcessId, process.ParentProcessId);
                    }
                }
            }

            RootMatch FindRoot(const uint32_t startPid)
            {
                if (startPid == 0)
                {
                    return {};
                }
                if (const auto cached = Memo.find(startPid); cached != Memo.end())
                {
                    return cached->second;
                }

                std::array<uint32_t, MaximumProcessTreeDepth> path{};
                size_t pathLen = 0;
                auto currentId = startPid;
                RootMatch resolved{};

                while (currentId != 0 && pathLen < MaximumProcessTreeDepth)
                {
                    if (Roots.contains(currentId))
                    {
                        resolved = { currentId, 0 };
                        break;
                    }
                    if (const auto cached = Memo.find(currentId); cached != Memo.end())
                    {
                        resolved = cached->second;
                        break;
                    }
                    path[pathLen++] = currentId;
                    const auto parent = Parents.find(currentId);
                    if (parent == Parents.end() || parent->second == currentId)
                    {
                        break;
                    }
                    currentId = parent->second;
                }

                for (size_t i = pathLen; i > 0; --i)
                {
                    const auto pid = path[i - 1];
                    if (resolved.RootPid != 0)
                    {
                        resolved.Depth += 1;
                    }
                    Memo.insert_or_assign(pid, resolved);
                }
                if (pathLen == 0)
                {
                    Memo.insert_or_assign(startPid, resolved);
                }
                return resolved;
            }
        };

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
        WorkspaceListeningPorts::PortsByRootProcess _mapListenersWithLookup(
            ProcessTreeLookup& lookup,
            const std::span<const WorkspaceListeningPorts::ListenerRecord> listeners)
        {
            WorkspaceListeningPorts::PortsByRootProcess result;
            for (const auto& listener : listeners)
            {
                if (listener.ProcessId == 0 || listener.Port == 0)
                {
                    continue;
                }
                if (const auto match = lookup.FindRoot(listener.ProcessId); match.RootPid != 0)
                {
                    result[match.RootPid].emplace_back(listener.Port);
                }
            }
            for (auto& [processId, ports] : result)
            {
                std::sort(ports.begin(), ports.end());
                ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
            }
            return result;
        }

        WorkspaceListeningPorts::ProcessActivityByRoot _mapActivityWithLookup(
            ProcessTreeLookup& lookup,
            const std::span<const WorkspaceListeningPorts::ProcessRecord> processes)
        {
            struct Candidate
            {
                int Tier{ 0 };
                size_t Depth{ 0 };
                bool NonCmdWrapper{ false };
                std::wstring Name;
                std::wstring CommandLine;
            };

            WorkspaceListeningPorts::ProcessActivityByRoot result;
            std::unordered_map<uint32_t, Candidate> bestCandidates;

            for (const auto& process : processes)
            {
                if (process.ProcessId == 0 || process.ExecutableName.empty())
                {
                    continue;
                }

                const auto match = lookup.FindRoot(process.ProcessId);
                if (match.RootPid == 0)
                {
                    continue;
                }

                auto& activity = result[match.RootPid];
                const auto lowerBase = _lowerBaseName(process.ExecutableName);

                if (match.Depth == 0)
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
                candidate.Depth = match.Depth;
                candidate.CommandLine = process.CommandLine.empty() ? process.ExecutableName : process.CommandLine;

                if (const auto agent = _detectAgentLabel(process.ExecutableName, process.CommandLine); !agent.empty())
                {
                    candidate.Tier = 3;
                    candidate.Name = std::wstring{ agent };
                    activity.HasActiveChild = true;
                }
                else if (match.Depth > 0 || !_isInteractiveShell(lowerBase))
                {
                    candidate.Tier = _isWrapperProcess(lowerBase) ? 1 : 2;
                    candidate.NonCmdWrapper = (lowerBase != L"cmd");
                    candidate.Name = _cleanDisplayName(process.ExecutableName);
                }
                else
                {
                    continue;
                }

                auto& best = bestCandidates[match.RootPid];
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
    }

    WorkspaceListeningPorts::PortsByRootProcess WorkspaceListeningPorts::MapListenersToRoots(
        const std::span<const uint32_t> rootProcessIds,
        const std::span<const ProcessRecord> processes,
        const std::span<const ListenerRecord> listeners)
    {
        ProcessTreeLookup lookup{ rootProcessIds, processes };
        return _mapListenersWithLookup(lookup, listeners);
    }

    WorkspaceListeningPorts::ProcessNamesByRoot WorkspaceListeningPorts::MapProcessNamesToRoots(
        const std::span<const uint32_t> rootProcessIds,
        const std::span<const ProcessRecord> processes)
    {
        ProcessTreeLookup lookup{ rootProcessIds, processes };
        ProcessNamesByRoot result;
        for (const auto& process : processes)
        {
            if (process.ProcessId == 0 || process.ExecutableName.empty())
            {
                continue;
            }
            if (const auto match = lookup.FindRoot(process.ProcessId); match.RootPid != 0)
            {
                result[match.RootPid].emplace_back(process.ExecutableName);
            }
        }
        return result;
    }

    WorkspaceListeningPorts::ProcessActivityByRoot WorkspaceListeningPorts::MapProcessActivityToRoots(
        const std::span<const uint32_t> rootProcessIds,
        const std::span<const ProcessRecord> processes)
    {
        ProcessTreeLookup lookup{ rootProcessIds, processes };
        return _mapActivityWithLookup(lookup, processes);
    }

    WorkspaceListeningPorts::Snapshot WorkspaceListeningPorts::Query(const std::span<const uint32_t> rootProcessIds) noexcept
    try
    {
        if (rootProcessIds.empty())
        {
            return { true, {}, {}, {} };
        }

        std::vector<ProcessRecord> processes;
        processes.reserve(512);
        if (!_queryProcesses(processes))
        {
            return {};
        }

        // Single shared process-tree lookup + memoized root resolution across command-line enrichment,
        // TCP listener attribution, and active child process classification.
        ProcessTreeLookup lookup{ rootProcessIds, processes };
        _pruneCommandLineCache(lookup.Parents);

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
            if (lookup.FindRoot(process.ProcessId).RootPid != 0)
            {
                process.CommandLine = _queryProcessCommandLineCached(process.ProcessId);
            }
        }

        std::vector<ListenerRecord> listeners;
        listeners.reserve(128);
        if (!_queryListeners(listeners))
        {
            return {};
        }

        return {
            true,
            _mapListenersWithLookup(lookup, listeners),
            {},
            _mapActivityWithLookup(lookup, processes),
        };
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return {};
    }
}
