// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskExecution
{
    enum class GitExecutableStatus
    {
        Succeeded,
        InvalidPath,
        Missing,
        UnsafePath,
        IdentityMismatch,
        PathUnavailable,
        ResourceFailure,
    };

    struct GitExecutableResult
    {
        GitExecutableStatus Status{ GitExecutableStatus::InvalidPath };
        std::optional<std::filesystem::path> Executable;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitExecutableStatus::Succeeded &&
                   Executable.has_value();
        }
    };

    [[nodiscard]] GitExecutableResult ValidateGitExecutable(
        const std::filesystem::path& executable) noexcept;
    [[nodiscard]] GitExecutableResult FindGitExecutable() noexcept;
}
