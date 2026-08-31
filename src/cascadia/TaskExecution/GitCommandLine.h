// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskExecution::details
{
    [[nodiscard]] std::optional<std::wstring> QuoteWindowsArgument(
        std::wstring_view argument) noexcept;
    [[nodiscard]] std::optional<std::wstring> BuildGitCommandLine(
        const std::filesystem::path& executable,
        std::span<const std::wstring> arguments) noexcept;
}
