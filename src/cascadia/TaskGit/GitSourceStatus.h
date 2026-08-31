// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitSourceSnapshot.h"

#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::TaskGit::details
{
    struct GitStatusParseResult
    {
        bool Succeeded{};
        GitSourceStatusSummary Summary;
        std::string Error;
    };

    struct GitPathListParseResult
    {
        bool Succeeded{};
        bool CountExceeded{};
        std::vector<std::string> Paths;
        std::string Error;
    };

    [[nodiscard]] GitStatusParseResult ParseGitStatusPorcelainV2(
        std::string_view output) noexcept;
    [[nodiscard]] GitPathListParseResult ParseGitNulPathList(
        std::string_view output,
        size_t maximumPaths) noexcept;
}
