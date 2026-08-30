// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeTerminalOutputRing.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr uint64_t Generation{ 7 };

        TerminalFrame _output(
            const uint64_t sequence,
            std::vector<uint8_t> payload,
            const uint64_t generation = Generation,
            const uint32_t flags = 0)
        {
            TerminalFrame frame;
            frame.Kind = TerminalFrameKind::Output;
            frame.Flags = flags;
            frame.Sequence = sequence;
            frame.Generation = generation;
            frame.Payload = std::move(payload);
            return frame;
        }

        RuntimeTerminalOutputRing _ring(
            const size_t maximumBytes = 6,
            const size_t maximumFrames = 3,
            const uint32_t maximumFrameBytes = 10)
        {
            return RuntimeTerminalOutputRing{
                {
                    Generation,
                    maximumBytes,
                    maximumFrames,
                    maximumFrameBytes,
                }
            };
        }
    }

    class RuntimeTerminalOutputRingTests
    {
        TEST_CLASS(RuntimeTerminalOutputRingTests);

        TEST_METHOD(RejectsInvalidConfiguration);
        TEST_METHOD(AppendTracksExactFrameAndByteBounds);
        TEST_METHOD(EvictionAdvancesConservativeCatchUpWatermark);
        TEST_METHOD(RejectedAppendNeverMutatesRing);
        TEST_METHOD(CatchUpClassifiesGenerationReplayResetAndAhead);
        TEST_METHOD(EndOfStreamMayRetainAnEmptyOutputFrame);
    };

    void RuntimeTerminalOutputRingTests::RejectsInvalidConfiguration()
    {
        const std::array configurations{
            RuntimeTerminalOutputRingConfiguration{ 0, 1, 1, 1 },
            RuntimeTerminalOutputRingConfiguration{ 1, 0, 1, 1 },
            RuntimeTerminalOutputRingConfiguration{ 1, 1, 0, 1 },
            RuntimeTerminalOutputRingConfiguration{ 1, 1, 1, 0 },
            RuntimeTerminalOutputRingConfiguration{
                1,
                1,
                1,
                MaximumTerminalFrameBytes + 1,
            },
        };
        for (const auto& configuration : configurations)
        {
            RuntimeTerminalOutputRing ring{ configuration };
            VERIFY_IS_FALSE(ring.Valid());
            VERIFY_IS_TRUE(
                ring.Append(_output(1, { 'x' })).Status ==
                RuntimeTerminalOutputAppendStatus::InvalidConfiguration);
            VERIFY_IS_TRUE(
                ring.CatchUp(configuration.Generation, 0).Status ==
                RuntimeTerminalCatchUpStatus::InvalidConfiguration);
        }
    }

    void RuntimeTerminalOutputRingTests::AppendTracksExactFrameAndByteBounds()
    {
        auto ring = _ring();
        VERIFY_IS_TRUE(ring.Valid());
        VERIFY_IS_TRUE(
            ring.Append(_output(2, { 'a', 'b' })).Status ==
            RuntimeTerminalOutputAppendStatus::Added);
        VERIFY_IS_TRUE(
            ring.Append(_output(5, { 'c', 'd' })).Status ==
            RuntimeTerminalOutputAppendStatus::Added);
        VERIFY_IS_TRUE(
            ring.Append(_output(9, { 'e', 'f' })).Status ==
            RuntimeTerminalOutputAppendStatus::Added);
        VERIFY_ARE_EQUAL(size_t{ 3 }, ring.RetainedFrames());
        VERIFY_ARE_EQUAL(size_t{ 6 }, ring.RetainedBytes());
        VERIFY_ARE_EQUAL(uint64_t{ 9 }, ring.LatestSequence());
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, ring.EvictedThroughSequence());
        VERIFY_IS_TRUE(ring.OldestRetainedSequence() == std::optional<uint64_t>{ 2 });
    }

    void RuntimeTerminalOutputRingTests::EvictionAdvancesConservativeCatchUpWatermark()
    {
        auto ring = _ring();
        (void)ring.Append(_output(2, { 'a', 'b' }));
        (void)ring.Append(_output(5, { 'c', 'd' }));
        (void)ring.Append(_output(9, { 'e', 'f' }));

        const auto countEviction = ring.Append(_output(12, { 'g' }));
        VERIFY_IS_TRUE(
            countEviction.Status ==
            RuntimeTerminalOutputAppendStatus::AddedWithEviction);
        VERIFY_ARE_EQUAL(size_t{ 1 }, countEviction.EvictedFrames);
        VERIFY_ARE_EQUAL(size_t{ 2 }, countEviction.EvictedBytes);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, ring.EvictedThroughSequence());
        VERIFY_ARE_EQUAL(size_t{ 3 }, ring.RetainedFrames());
        VERIFY_ARE_EQUAL(size_t{ 5 }, ring.RetainedBytes());

        const auto byteEviction = ring.Append(
            _output(15, { 'h', 'i', 'j', 'k', 'l' }));
        VERIFY_IS_TRUE(
            byteEviction.Status ==
            RuntimeTerminalOutputAppendStatus::AddedWithEviction);
        VERIFY_ARE_EQUAL(size_t{ 2 }, byteEviction.EvictedFrames);
        VERIFY_ARE_EQUAL(size_t{ 4 }, byteEviction.EvictedBytes);
        VERIFY_ARE_EQUAL(uint64_t{ 9 }, ring.EvictedThroughSequence());
        VERIFY_ARE_EQUAL(size_t{ 2 }, ring.RetainedFrames());
        VERIFY_ARE_EQUAL(size_t{ 6 }, ring.RetainedBytes());
        VERIFY_IS_TRUE(ring.OldestRetainedSequence() == std::optional<uint64_t>{ 12 });
    }

    void RuntimeTerminalOutputRingTests::RejectedAppendNeverMutatesRing()
    {
        auto ring = _ring(6, 3, 10);
        VERIFY_IS_TRUE(
            ring.Append(_output(4, { 'a' })).Status ==
            RuntimeTerminalOutputAppendStatus::Added);

        auto input = _output(5, { 'b' });
        input.Kind = TerminalFrameKind::Input;
        VERIFY_IS_TRUE(
            ring.Append(std::move(input)).Status ==
            RuntimeTerminalOutputAppendStatus::InvalidFrame);
        VERIFY_IS_TRUE(
            ring.Append(_output(5, { 'b' }, Generation + 1)).Status ==
            RuntimeTerminalOutputAppendStatus::StaleGeneration);
        VERIFY_IS_TRUE(
            ring.Append(_output(4, { 'b' })).Status ==
            RuntimeTerminalOutputAppendStatus::NonMonotonicSequence);
        VERIFY_IS_TRUE(
            ring.Append(_output(5, std::vector<uint8_t>(7, 'x'))).Status ==
            RuntimeTerminalOutputAppendStatus::FrameExceedsRing);
        VERIFY_IS_TRUE(
            ring.Append(_output(5, {})).Status ==
            RuntimeTerminalOutputAppendStatus::InvalidFrame);

        VERIFY_ARE_EQUAL(uint64_t{ 4 }, ring.LatestSequence());
        VERIFY_ARE_EQUAL(size_t{ 1 }, ring.RetainedFrames());
        VERIFY_ARE_EQUAL(size_t{ 1 }, ring.RetainedBytes());
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, ring.EvictedThroughSequence());
    }

    void RuntimeTerminalOutputRingTests::CatchUpClassifiesGenerationReplayResetAndAhead()
    {
        auto ring = _ring();
        VERIFY_IS_TRUE(
            ring.CatchUp(Generation, 0).Status ==
            RuntimeTerminalCatchUpStatus::UpToDate);
        VERIFY_IS_TRUE(
            ring.CatchUp(Generation + 1, 0).Status ==
            RuntimeTerminalCatchUpStatus::StaleGeneration);
        VERIFY_IS_TRUE(
            ring.CatchUp(Generation, 1).Status ==
            RuntimeTerminalCatchUpStatus::AcknowledgementAhead);

        (void)ring.Append(_output(2, { 'a', 'b' }));
        (void)ring.Append(_output(5, { 'c', 'd' }));
        (void)ring.Append(_output(9, { 'e', 'f' }));
        (void)ring.Append(_output(12, { 'g' }));

        const auto reset = ring.CatchUp(Generation, 1);
        VERIFY_IS_TRUE(reset.Status == RuntimeTerminalCatchUpStatus::ResetRequired);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, reset.EvictedThroughSequence);
        VERIFY_IS_TRUE(reset.Frames.empty());

        const auto replay = ring.CatchUp(Generation, 2);
        VERIFY_IS_TRUE(replay.Status == RuntimeTerminalCatchUpStatus::Replay);
        VERIFY_ARE_EQUAL(size_t{ 3 }, replay.Frames.size());
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, replay.Frames[0].Sequence);
        VERIFY_ARE_EQUAL(uint64_t{ 12 }, replay.Frames[2].Sequence);

        const auto gapReplay = ring.CatchUp(Generation, 6);
        VERIFY_IS_TRUE(gapReplay.Status == RuntimeTerminalCatchUpStatus::Replay);
        VERIFY_ARE_EQUAL(size_t{ 2 }, gapReplay.Frames.size());
        VERIFY_ARE_EQUAL(uint64_t{ 9 }, gapReplay.Frames[0].Sequence);
        VERIFY_IS_TRUE(
            ring.CatchUp(Generation, 12).Status ==
            RuntimeTerminalCatchUpStatus::UpToDate);
        VERIFY_IS_TRUE(
            ring.CatchUp(Generation, 13).Status ==
            RuntimeTerminalCatchUpStatus::AcknowledgementAhead);
    }

    void RuntimeTerminalOutputRingTests::EndOfStreamMayRetainAnEmptyOutputFrame()
    {
        auto ring = _ring();
        const auto endFlag = static_cast<uint32_t>(TerminalFrameFlag::EndOfStream);
        const auto appended = ring.Append(_output(1, {}, Generation, endFlag));
        VERIFY_IS_TRUE(
            appended.Status == RuntimeTerminalOutputAppendStatus::Added);
        VERIFY_ARE_EQUAL(size_t{ 1 }, ring.RetainedFrames());
        VERIFY_ARE_EQUAL(size_t{ 0 }, ring.RetainedBytes());
        const auto replay = ring.CatchUp(Generation, 0);
        VERIFY_IS_TRUE(replay.Status == RuntimeTerminalCatchUpStatus::Replay);
        VERIFY_ARE_EQUAL(size_t{ 1 }, replay.Frames.size());
        VERIFY_ARE_EQUAL(endFlag, replay.Frames.front().Flags);
    }
}
