// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitCommandTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskExecution::details
{
    [[nodiscard]] GitCommandResult RunGitProcess(
        const GitCommandRequest& request,
        const std::filesystem::path& executable,
        const std::filesystem::path& workingDirectory,
        std::wstring commandLine,
        std::vector<wchar_t> environment) noexcept;
}
