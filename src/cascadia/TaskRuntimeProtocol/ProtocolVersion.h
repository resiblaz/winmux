// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstddef>
#include <cstdint>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr uint16_t CurrentProtocolMajor{ 1 };
    inline constexpr uint16_t MinimumProtocolMinor{ 0 };
    inline constexpr uint16_t CurrentProtocolMinor{ 0 };

    inline constexpr uint32_t MinimumControlFrameBytes{ 256 };
    inline constexpr uint32_t MaximumControlFrameBytes{ 1024 * 1024 };
    inline constexpr uint32_t MaximumTerminalFrameBytes{ 256 * 1024 };
    inline constexpr size_t MaximumTransportFeedBytes{ 64 * 1024 };
    inline constexpr size_t MaximumFramesPerFeed{ 64 };

    struct ProtocolVersionRange
    {
        uint16_t Major{ CurrentProtocolMajor };
        uint16_t MinimumMinor{ MinimumProtocolMinor };
        uint16_t PreferredMinor{ CurrentProtocolMinor };
    };
}
