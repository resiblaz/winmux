// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <filesystem>
#include <optional>

namespace Microsoft::Terminal::TaskRuntime
{
    [[nodiscard]] bool IsSafeCodexExecutable(const std::filesystem::path& candidate) noexcept;
    [[nodiscard]] std::optional<std::filesystem::path> DiscoverCodexExecutable() noexcept;
}
