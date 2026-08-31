// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "DirectoryIdentity.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskExecution
{
    inline constexpr uint32_t MaximumGitCommandTimeoutMilliseconds{ 30000 };
    inline constexpr size_t MaximumGitArgumentCount{ 128 };
    inline constexpr size_t MaximumGitArgumentCharacters{ 32759 };
    inline constexpr size_t MaximumGitCommandLineCharacters{ 32766 };
    inline constexpr size_t MaximumGitEnvironmentCharacters{ 32767 };
    inline constexpr size_t DefaultGitStdoutBytes{ 4 * 1024 * 1024 };
    inline constexpr size_t DefaultGitStderrBytes{ 1024 * 1024 };
    inline constexpr size_t MaximumGitOutputBytes{ 16 * 1024 * 1024 };

    enum class GitCommandStatus
    {
        Completed,
        Cancelled,
        TimedOut,
        OutputLimitExceeded,
        InvalidUtf8,
        InvalidRequest,
        ExecutableUnavailable,
        LaunchFailed,
        IoFailure,
        SystemError,
        ResourceFailure,
    };

    struct GitEnvironmentOverride
    {
        std::wstring Name;
        std::wstring Value;
    };

    struct GitCommandRequest
    {
        std::filesystem::path GitExecutable;
        CanonicalDirectoryIdentity WorkingDirectory;
        std::vector<std::wstring> Arguments;
        std::vector<GitEnvironmentOverride> Environment;
        HANDLE CancellationEvent{};
        uint32_t TimeoutMilliseconds{ MaximumGitCommandTimeoutMilliseconds };
        size_t MaximumStdoutBytes{ DefaultGitStdoutBytes };
        size_t MaximumStderrBytes{ DefaultGitStderrBytes };
    };

    struct GitCommandResult
    {
        GitCommandStatus Status{ GitCommandStatus::InvalidRequest };
        DWORD SystemError{};
        DWORD ExitCode{};
        std::string Stdout;
        std::string Stderr;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitCommandStatus::Completed && ExitCode == 0;
        }
    };
}
