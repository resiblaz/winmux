// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitCommandTypes.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::TaskExecution::details
{
    struct GitEnvironmentResult
    {
        bool Succeeded{};
        std::vector<wchar_t> Block;
        std::string Error;
    };

    [[nodiscard]] bool IsAllowedGitEnvironmentOverride(
        std::wstring_view name) noexcept;
    [[nodiscard]] GitEnvironmentResult BuildSanitizedGitEnvironment(
        std::span<const GitEnvironmentOverride> overrides) noexcept;
}
