// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolVersion.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    enum class FrameDecodeStatus
    {
        NeedMoreData,
        ProducedFrames,
        Malformed,
        Oversized,
        ResourceLimit,
    };

    struct ControlFrameDecodeResult
    {
        FrameDecodeStatus Status{ FrameDecodeStatus::NeedMoreData };
        std::vector<std::string> Payloads;
        std::string Error;

        [[nodiscard]] bool Failed() const noexcept
        {
            return Status == FrameDecodeStatus::Malformed ||
                   Status == FrameDecodeStatus::Oversized ||
                   Status == FrameDecodeStatus::ResourceLimit;
        }
    };

    struct ControlFrameEncodeResult
    {
        bool Succeeded{};
        std::vector<uint8_t> Bytes;
        std::string Error;
    };

    class ControlFrameDecoder final
    {
    public:
        explicit ControlFrameDecoder(uint32_t maximumPayloadBytes = MaximumControlFrameBytes);

        [[nodiscard]] ControlFrameDecodeResult Feed(std::span<const uint8_t> bytes);
        [[nodiscard]] bool Failed() const noexcept;
        [[nodiscard]] bool ReconfigureMaximumPayloadBytes(uint32_t maximumPayloadBytes) noexcept;
        void Reset() noexcept;

    private:
        uint32_t _maximumPayloadBytes{};
        std::vector<uint8_t> _buffer;
        bool _failed{};
    };

    [[nodiscard]] ControlFrameEncodeResult EncodeControlFrame(
        std::string_view payload,
        uint32_t maximumPayloadBytes = MaximumControlFrameBytes);
}
