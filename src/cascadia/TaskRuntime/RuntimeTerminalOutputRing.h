// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TerminalFrame.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr size_t DefaultRuntimeTerminalOutputRingBytes{
        4 * 1024 * 1024
    };
    inline constexpr size_t DefaultRuntimeTerminalOutputRingFrames{ 4096 };

    struct RuntimeTerminalOutputRingConfiguration
    {
        uint64_t Generation{};
        size_t MaximumBytes{ DefaultRuntimeTerminalOutputRingBytes };
        size_t MaximumFrames{ DefaultRuntimeTerminalOutputRingFrames };
        uint32_t MaximumFrameBytes{
            Microsoft::Terminal::TaskRuntimeProtocol::MaximumTerminalFrameBytes
        };
    };

    enum class RuntimeTerminalOutputAppendStatus
    {
        Added,
        AddedWithEviction,
        InvalidConfiguration,
        InvalidFrame,
        StaleGeneration,
        NonMonotonicSequence,
        FrameExceedsRing,
        ResourceFailure,
    };

    struct RuntimeTerminalOutputAppendResult
    {
        RuntimeTerminalOutputAppendStatus Status{
            RuntimeTerminalOutputAppendStatus::InvalidConfiguration
        };
        size_t EvictedFrames{};
        size_t EvictedBytes{};
    };

    enum class RuntimeTerminalCatchUpStatus
    {
        UpToDate,
        Replay,
        ResetRequired,
        AcknowledgementAhead,
        StaleGeneration,
        InvalidConfiguration,
        ResourceFailure,
    };

    struct RuntimeTerminalCatchUpResult
    {
        RuntimeTerminalCatchUpStatus Status{
            RuntimeTerminalCatchUpStatus::InvalidConfiguration
        };
        uint64_t EvictedThroughSequence{};
        uint64_t LatestSequence{};
        std::vector<Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame> Frames;
    };

    class RuntimeTerminalOutputRing final
    {
    public:
        explicit RuntimeTerminalOutputRing(
            RuntimeTerminalOutputRingConfiguration configuration) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeTerminalOutputAppendResult Append(
            Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame frame);
        [[nodiscard]] RuntimeTerminalCatchUpResult CatchUp(
            uint64_t generation,
            uint64_t acknowledgedSequence) const;

        [[nodiscard]] uint64_t Generation() const noexcept;
        [[nodiscard]] uint64_t LatestSequence() const noexcept;
        [[nodiscard]] uint64_t EvictedThroughSequence() const noexcept;
        [[nodiscard]] std::optional<uint64_t> OldestRetainedSequence() const noexcept;
        [[nodiscard]] size_t RetainedFrames() const noexcept;
        [[nodiscard]] size_t RetainedBytes() const noexcept;

    private:
        RuntimeTerminalOutputRingConfiguration _configuration;
        std::deque<Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame> _frames;
        size_t _retainedBytes{};
        uint64_t _latestSequence{};
        uint64_t _evictedThroughSequence{};
        bool _valid{};
    };
}
