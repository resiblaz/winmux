// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    [[nodiscard]] std::optional<std::string> NewRuntimeUuid();
    [[nodiscard]] std::optional<std::array<uint8_t, 32>> RuntimeSha256(
        std::span<const uint8_t> input);
    [[nodiscard]] std::optional<std::string> RuntimeSha256Hex(
        std::span<const uint8_t> input);
}
