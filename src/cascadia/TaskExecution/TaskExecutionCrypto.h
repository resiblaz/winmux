// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace Microsoft::Terminal::TaskExecution::details
{
    [[nodiscard]] std::optional<std::array<uint8_t, 32>> TaskExecutionSha256(
        std::span<const uint8_t> payload) noexcept;
    [[nodiscard]] std::string LowerHex(
        std::span<const uint8_t> payload);
}
