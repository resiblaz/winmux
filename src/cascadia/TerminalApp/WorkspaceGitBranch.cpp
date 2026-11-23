// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceGitBranch.h"

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

    bool WorkspaceGitBranch::IsSupportedDirectorySyntax(const std::wstring_view directory) noexcept
    {
        if (directory.size() < 3 || directory.size() > 32767)
        {
            return false;
        }

        const auto drive = directory[0];
        const auto isAsciiDrive = (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
        if (!isAsciiDrive ||
            directory[1] != L':' ||
            (directory[2] != L'\\' && directory[2] != L'/') ||
            directory.find(L"://") != std::wstring_view::npos)
        {
            return false;
        }

        for (const auto ch : directory)
        {
            if (ch < L' ' || ch == 0x7f)
            {
                return false;
            }
        }

        size_t index = 3;
        while (index < directory.size())
        {
            while (index < directory.size() && (directory[index] == L'\\' || directory[index] == L'/'))
            {
                ++index;
            }
            const auto start = index;
            while (index < directory.size() && directory[index] != L'\\' && directory[index] != L'/')
            {
                ++index;
            }
            if (directory.substr(start, index - start) == L"..")
            {
                return false;
            }
        }

        return true;
    }

    bool WorkspaceGitBranch::IsSupportedDirectory(const std::wstring_view directory) noexcept
    {
        if (!IsSupportedDirectorySyntax(directory))
        {
            return false;
        }

        const std::array<wchar_t, 4> driveRoot{ directory[0], L':', L'\\', L'\0' };
        if (GetDriveTypeW(driveRoot.data()) == DRIVE_REMOTE)
        {
            return false;
        }

        try
        {
            const std::wstring path{ directory };
            const auto attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && WI_IsFlagSet(attributes, FILE_ATTRIBUTE_DIRECTORY);
        }
        catch (...)
        {
            return false;
        }
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
        constexpr DWORD PipeBufferSize{ 64 * 1024 };
        constexpr size_t MaximumOutputBytes{ 8192 };

        SECURITY_ATTRIBUTES securityAttributes{
            .nLength = sizeof(SECURITY_ATTRIBUTES),
            .lpSecurityDescriptor = nullptr,
            .bInheritHandle = TRUE,
        };
        wil::unique_handle readPipe;
        wil::unique_handle writePipe;
        wil::unique_handle nullInput{ CreateFileW(L"NUL",
                                                   GENERIC_READ,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                   &securityAttributes,
                                                   OPEN_EXISTING,
                                                   FILE_ATTRIBUTE_NORMAL,
                                                   nullptr) };
        if (!nullInput ||
            !CreatePipe(readPipe.addressof(), writePipe.addressof(), &securityAttributes, PipeBufferSize) ||
            !SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0))
        {
            return {};
        }

        STARTUPINFOEXW startupInfo{};
        startupInfo.StartupInfo.cb = sizeof(startupInfo);
        startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.StartupInfo.hStdInput = nullInput.get();
        startupInfo.StartupInfo.hStdOutput = writePipe.get();
        startupInfo.StartupInfo.hStdError = writePipe.get();

        SIZE_T attributeListSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);
        if (attributeListSize == 0)
        {
            return {};
        }
        std::vector<std::byte> attributeListStorage(attributeListSize);
        startupInfo.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());
        if (!InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &attributeListSize))
        {
            return {};
        }
        const auto deleteAttributeList = wil::scope_exit([&]() {
            DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        });

        HANDLE inheritedHandles[]{ nullInput.get(), writePipe.get() };
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

        std::wstring commandLine{ L"git.exe -c core.quotepath=false rev-parse --show-toplevel --abbrev-ref HEAD" };
        std::wstring workingDirectory{ directory };

        PROCESS_INFORMATION processInformation{};
        if (!CreateProcessW(nullptr,
                            commandLine.data(),
                            nullptr,
                            nullptr,
                            TRUE,
                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr,
                            workingDirectory.c_str(),
                            &startupInfo.StartupInfo,
                            &processInformation))
        {
            return {};
        }

        wil::unique_handle process{ processInformation.hProcess };
        wil::unique_handle thread{ processInformation.hThread };
        writePipe.reset();
        nullInput.reset();

        const auto effectiveTimeout = std::max(timeout, std::chrono::milliseconds{ 1 });
        const auto deadline = std::chrono::steady_clock::now() + effectiveTimeout;
        std::string output;
        std::array<char, 1024> buffer{};
        bool processExited = false;
        bool outputExceeded = false;

        for (;;)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr))
            {
                const auto lastError = GetLastError();
                if (lastError == ERROR_BROKEN_PIPE)
                {
                    break;
                }
                TerminateProcess(process.get(), ERROR_GEN_FAILURE);
                WaitForSingleObject(process.get(), 1000);
                return {};
            }

            if (available > 0)
            {
                DWORD bytesRead = 0;
                const auto toRead = std::min<DWORD>(available, gsl::narrow<DWORD>(buffer.size()));
                if (!ReadFile(readPipe.get(), buffer.data(), toRead, &bytesRead, nullptr) || bytesRead == 0)
                {
                    break;
                }
                if (bytesRead > MaximumOutputBytes - std::min(output.size(), MaximumOutputBytes))
                {
                    outputExceeded = true;
                    break;
                }
                output.append(buffer.data(), bytesRead);
                continue;
            }

            if (processExited)
            {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                TerminateProcess(process.get(), ERROR_TIMEOUT);
                WaitForSingleObject(process.get(), 1000);
                return {};
            }

            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const auto sliceMs = gsl::narrow<DWORD>(std::clamp(remainingMs, 1ll, 10ll));
            const auto waitResult = WaitForSingleObject(process.get(), sliceMs);
            if (waitResult == WAIT_OBJECT_0)
            {
                processExited = true;
            }
            else if (waitResult != WAIT_TIMEOUT)
            {
                TerminateProcess(process.get(), ERROR_GEN_FAILURE);
                WaitForSingleObject(process.get(), 1000);
                return {};
            }
        }

        if (outputExceeded)
        {
            TerminateProcess(process.get(), ERROR_BUFFER_OVERFLOW);
            WaitForSingleObject(process.get(), 1000);
            return {};
        }

        if (!processExited)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto remainingMs = now < deadline ?
                                         std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count(), 1ll) :
                                         0ll;
            if (WaitForSingleObject(process.get(), gsl::narrow<DWORD>(remainingMs)) != WAIT_OBJECT_0)
            {
                TerminateProcess(process.get(), ERROR_TIMEOUT);
                WaitForSingleObject(process.get(), 1000);
                return {};
            }
        }

        DWORD exitCode = ERROR_GEN_FAILURE;
        if (!GetExitCodeProcess(process.get(), &exitCode) || exitCode != 0)
        {
            return {};
        }

        return ParseOutputForTests(output);
    }
}
