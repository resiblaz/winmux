// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ControlFrame.h"
#include "ProtocolVersion.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr uint32_t TerminalFrameMagic{ 0x31525454 }; // TTR1 on the wire
    inline constexpr uint16_t TerminalFrameHeaderVersion{ 1 };
    inline constexpr size_t TerminalFrameHeaderBytes{ 32 };
    inline constexpr uint32_t MaximumTerminalInputBytes{ 64 * 1024 };
    inline constexpr uint32_t MaximumTerminalMetadataBytes{ 64 * 1024 };
    inline constexpr uint32_t MaximumTerminalDimension{ 10000 };

    enum class TerminalFrameKind : uint16_t
    {
        Hello = 1,
        Attach = 2,
        Reset = 3,
        Output = 4,
        Input = 5,
        Resize = 6,
        Acknowledge = 7,
        Ping = 8,
        Pong = 9,
        Error = 10,
    };

    enum class TerminalFrameFlag : uint32_t
    {
        ResetBeforePayload = 1u << 0,
        EndOfStream = 1u << 1,
    };

    struct TerminalFrame
    {
        TerminalFrameKind Kind{ TerminalFrameKind::Hello };
        uint32_t Flags{};
        uint64_t Sequence{};
        uint64_t Generation{};
        std::vector<uint8_t> Payload;
    };

    struct TerminalFrameDecodeResult
    {
        FrameDecodeStatus Status{ FrameDecodeStatus::NeedMoreData };
        std::vector<TerminalFrame> Frames;
        std::string Error;

        [[nodiscard]] bool Failed() const noexcept
        {
            return Status == FrameDecodeStatus::Malformed ||
                   Status == FrameDecodeStatus::Oversized ||
                   Status == FrameDecodeStatus::ResourceLimit;
        }
    };

    struct TerminalFrameEncodeResult
    {
        bool Succeeded{};
        std::vector<uint8_t> Bytes;
        std::string Error;
    };

    class TerminalFrameDecoder final
    {
    public:
        explicit TerminalFrameDecoder(uint32_t maximumPayloadBytes = MaximumTerminalFrameBytes);

        [[nodiscard]] TerminalFrameDecodeResult Feed(std::span<const uint8_t> bytes);
        [[nodiscard]] bool Failed() const noexcept;
        void Reset() noexcept;

    private:
        uint32_t _maximumPayloadBytes{};
        std::vector<uint8_t> _buffer;
        uint64_t _lastSequence{};
        bool _failed{};
    };

    [[nodiscard]] bool ValidateTerminalFrame(
        const TerminalFrame& frame,
        uint32_t maximumPayloadBytes,
        std::string& error);

    [[nodiscard]] TerminalFrameEncodeResult EncodeTerminalFrame(
        const TerminalFrame& frame,
        uint32_t maximumPayloadBytes = MaximumTerminalFrameBytes);

    [[nodiscard]] std::array<uint8_t, 8> EncodeResizePayload(uint32_t columns, uint32_t rows) noexcept;
    [[nodiscard]] bool DecodeResizePayload(
        std::span<const uint8_t> payload,
        uint32_t& columns,
        uint32_t& rows) noexcept;

    [[nodiscard]] std::array<uint8_t, 8> EncodeAcknowledgementPayload(uint64_t sequence) noexcept;
    [[nodiscard]] bool DecodeAcknowledgementPayload(
        std::span<const uint8_t> payload,
        uint64_t& sequence) noexcept;
}
