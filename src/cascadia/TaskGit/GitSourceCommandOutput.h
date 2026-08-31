// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitCommandTypes.h"
#include "GitRepositoryInspector.h"

#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskGit::details
{
    [[nodiscard]] std::optional<std::string> ParseGitSourceLine(
        const Microsoft::Terminal::TaskExecution::GitCommandResult& result,
        bool allowEmpty = false);
    [[nodiscard]] bool IsGitSourceObjectId(
        std::string_view value,
        Microsoft::Terminal::TaskExecution::GitObjectFormat format) noexcept;
    [[nodiscard]] std::wstring WidenGitSourceAscii(std::string_view value);
}
