// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeWindowsProcessApiInternal.h"

#include <filesystem>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeExecutablePathStatus
    {
        Succeeded,
        InvalidPath,
        MissingPath,
        UntrustedAttributes,
        ResourceFailure,
    };

    struct RuntimeExecutablePathResult
    {
        RuntimeExecutablePathStatus Status{ RuntimeExecutablePathStatus::InvalidPath };
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeExecutablePathStatus::Succeeded;
        }
    };

    [[nodiscard]] RuntimeExecutablePathResult ValidateRuntimeExecutablePath(
        const std::filesystem::path& executablePath,
        const std::filesystem::path& trustedRoot,
        details::IRuntimeWindowsProcessApi& api) noexcept;
}
