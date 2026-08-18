// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceGitBranch.h"

#include <Shlwapi.h>

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        struct CacheEntry
        {
            WorkspaceGitBranchResult Result;
            std::chrono::steady_clock::time_point QueriedAt;
        };

        constexpr size_t MaximumCacheEntries{ 128 };
        std::mutex _cacheMutex;
        std::unordered_map<std::wstring, CacheEntry> _cache;

        std::wstring _cacheKey(std::wstring_view directory)
        {
            std::wstring key{ directory };
            std::replace(key.begin(), key.end(), L'/', L'\\');
            while (key.size() > 3 && key.back() == L'\\')
            {
                key.pop_back();
            }
            CharLowerBuffW(key.data(), gsl::narrow<DWORD>(key.size()));
            return key;
        }

        std::string_view _trimLine(std::string_view line) noexcept
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            {
                line.remove_suffix(1);
            }
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            {
                line.remove_prefix(1);
            }
            return line;
        }

        void _trimCache()
        {
            if (_cache.size() < MaximumCacheEntries)
            {
                return;
            }

            const auto oldest = std::min_element(_cache.begin(), _cache.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.second.QueriedAt < rhs.second.QueriedAt;
            });
            if (oldest != _cache.end())
            {
                _cache.erase(oldest);
            }
        }
    }

    bool WorkspaceGitBranch::IsSupportedDirectory(const std::wstring_view directory) noexcept
    {
        if (directory.empty())
        {
            return false;
        }

        const std::wstring path{ directory };
        if (path.find(L"://") != std::wstring::npos || PathIsUNCW(path.c_str()))
        {
            return false;
        }

        const auto attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && WI_IsFlagSet(attributes, FILE_ATTRIBUTE_DIRECTORY);
    }

    WorkspaceGitBranchResult WorkspaceGitBranch::QueryCached(const std::wstring_view directory,
                                                             const std::chrono::milliseconds timeout,
                                                             const std::chrono::milliseconds maximumAge)
    {
        if (!IsSupportedDirectory(directory))
        {
            return {};
        }

        const auto key = _cacheKey(directory);
        const auto now = std::chrono::steady_clock::now();
        {
            const std::scoped_lock lock{ _cacheMutex };
            if (const auto found = _cache.find(key); found != _cache.end() && now - found->second.QueriedAt <= maximumAge)
            {
                auto result = found->second.Result;
                result.FromCache = true;
                return result;
            }
        }

        auto result = _Query(directory, timeout);
        {
            const std::scoped_lock lock{ _cacheMutex };
            _trimCache();
            _cache.insert_or_assign(key, CacheEntry{ result, now });
        }
        return result;
    }

    WorkspaceGitBranchResult WorkspaceGitBranch::ParseOutputForTests(const std::string_view output)
    {
        const auto firstNewline = output.find('\n');
        if (firstNewline == std::string_view::npos)
        {
            return {};
        }

        const auto secondNewline = output.find('\n', firstNewline + 1);
        const auto repositoryRoot = _trimLine(output.substr(0, firstNewline));
        const auto branch = _trimLine(output.substr(firstNewline + 1,
                                                    secondNewline == std::string_view::npos ? std::string_view::npos : secondNewline - firstNewline - 1));
        if (repositoryRoot.empty() || branch.empty())
        {
            return {};
        }

        return {
            .RepositoryRoot = winrt::to_hstring(repositoryRoot),
            .Branch = winrt::to_hstring(branch),
            .IsRepository = true,
        };
    }

    void WorkspaceGitBranch::ClearCacheForTests()
    {
        const std::scoped_lock lock{ _cacheMutex };
        _cache.clear();
    }

    WorkspaceGitBranchResult WorkspaceGitBranch::_Query(const std::wstring_view directory, const std::chrono::milliseconds timeout)
    {
        SECURITY_ATTRIBUTES securityAttributes{
            .nLength = sizeof(SECURITY_ATTRIBUTES),
            .lpSecurityDescriptor = nullptr,
            .bInheritHandle = TRUE,
        };
        wil::unique_handle readPipe;
        wil::unique_handle writePipe;
        if (!CreatePipe(readPipe.addressof(), writePipe.addressof(), &securityAttributes, 0) ||
            !SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0))
        {
            return {};
        }

        STARTUPINFOEXW startupInfo{};
        startupInfo.StartupInfo.cb = sizeof(startupInfo);
        startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.StartupInfo.hStdOutput = writePipe.get();
        startupInfo.StartupInfo.hStdError = writePipe.get();

        SIZE_T attributeListSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);
        std::vector<std::byte> attributeListStorage(attributeListSize);
        startupInfo.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());
        if (!InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &attributeListSize))
        {
            return {};
        }
        const auto deleteAttributeList = wil::scope_exit([&]() {
            DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        });

        HANDLE inheritedHandles[]{ writePipe.get() };
        if (!UpdateProcThreadAttribute(startupInfo.lpAttributeList,
                                       0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inheritedHandles,
                                       sizeof(inheritedHandles),
                                       nullptr,
                                       nullptr))
        {
            return {};
        }

        std::wstring commandLine{ L"git.exe -c core.quotepath=false -C \"" };
        commandLine.append(directory);
        commandLine.append(L"\" rev-parse --show-toplevel --abbrev-ref HEAD");

        PROCESS_INFORMATION processInformation{};
        if (!CreateProcessW(nullptr,
                            commandLine.data(),
                            nullptr,
                            nullptr,
                            TRUE,
                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr,
                            nullptr,
                            &startupInfo.StartupInfo,
                            &processInformation))
        {
            return {};
        }

        wil::unique_handle process{ processInformation.hProcess };
        wil::unique_handle thread{ processInformation.hThread };
        writePipe.reset();

        const auto waitResult = WaitForSingleObject(process.get(), gsl::narrow<DWORD>(std::max(timeout.count(), 1ll)));
        if (waitResult != WAIT_OBJECT_0)
        {
            TerminateProcess(process.get(), ERROR_TIMEOUT);
            WaitForSingleObject(process.get(), 1000);
            return {};
        }

        DWORD exitCode = ERROR_GEN_FAILURE;
        if (!GetExitCodeProcess(process.get(), &exitCode) || exitCode != 0)
        {
            return {};
        }

        std::string output;
        std::array<char, 1024> buffer{};
        for (;;)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(readPipe.get(), buffer.data(), gsl::narrow<DWORD>(buffer.size()), &bytesRead, nullptr) || bytesRead == 0)
            {
                break;
            }
            output.append(buffer.data(), bytesRead);
            if (output.size() > 8192)
            {
                return {};
            }
        }

        return ParseOutputForTests(output);
    }
}
