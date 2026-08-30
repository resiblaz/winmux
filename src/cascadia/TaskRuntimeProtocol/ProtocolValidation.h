// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <span>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    [[nodiscard]] bool IsCanonicalProtocolUuid(std::string_view value) noexcept;
    [[nodiscard]] bool IsValidRuntimeIdentity(std::string_view value) noexcept;
    [[nodiscard]] bool IsValidUtf8(std::span<const uint8_t> value) noexcept;
    [[nodiscard]] bool IsValidUtf8(std::string_view value) noexcept;
}
