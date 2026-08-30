// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TerminalFrame.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        TerminalFrame _frame(
            const TerminalFrameKind kind,
            const uint64_t sequence,
            const uint64_t generation,
            std::vector<uint8_t> payload = {})
        {
            TerminalFrame frame;
            frame.Kind = kind;
            frame.Sequence = sequence;
            frame.Generation = generation;
            frame.Payload = std::move(payload);
            return frame;
        }

        void _append(std::vector<uint8_t>& target, const std::vector<uint8_t>& source)
        {
            target.insert(target.end(), source.begin(), source.end());
        }
    }

    class TerminalFrameTests
    {
        TEST_CLASS(TerminalFrameTests);

        TEST_METHOD(BinaryOutputRoundTripsAcrossFragmentedFeeds);
        TEST_METHOD(CoalescedFramesRequireIncreasingSequence);
        TEST_METHOD(ResizePayloadEnforcesDimensionsAndFrameShape);
        TEST_METHOD(RejectsInvalidMagicVersionKindFlagsAndGeneration);
        TEST_METHOD(RejectsOversizedFeedsPayloadsAndFrameBursts);
        TEST_METHOD(FailedDecoderCanBeExplicitlyReset);
    };

    void TerminalFrameTests::BinaryOutputRoundTripsAcrossFragmentedFeeds()
    {
        auto frame = _frame(TerminalFrameKind::Output, 1, 7, { 0x1b, '[', '3', '1', 'm', 0xff });
        frame.Flags = static_cast<uint32_t>(TerminalFrameFlag::ResetBeforePayload);
        const auto encoded = EncodeTerminalFrame(frame);
        VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_ARE_EQUAL(TerminalFrameHeaderBytes + frame.Payload.size(), encoded.Bytes.size());
        VERIFY_ARE_EQUAL(uint8_t{ 'T' }, encoded.Bytes[0]);
        VERIFY_ARE_EQUAL(uint8_t{ 'T' }, encoded.Bytes[1]);
        VERIFY_ARE_EQUAL(uint8_t{ 'R' }, encoded.Bytes[2]);
        VERIFY_ARE_EQUAL(uint8_t{ '1' }, encoded.Bytes[3]);

        TerminalFrameDecoder decoder;
        auto decoded = decoder.Feed(std::span{ encoded.Bytes }.first(17));
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::NeedMoreData);
        decoded = decoder.Feed(std::span{ encoded.Bytes }.subspan(17, 18));
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::NeedMoreData);
        decoded = decoder.Feed(std::span{ encoded.Bytes }.subspan(35));
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::ProducedFrames);
        VERIFY_ARE_EQUAL(size_t{ 1 }, decoded.Frames.size());
        VERIFY_IS_TRUE(decoded.Frames[0].Kind == TerminalFrameKind::Output);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, decoded.Frames[0].Sequence);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, decoded.Frames[0].Generation);
        VERIFY_IS_TRUE(decoded.Frames[0].Payload == frame.Payload);
    }

    void TerminalFrameTests::CoalescedFramesRequireIncreasingSequence()
    {
        const auto hello = EncodeTerminalFrame(_frame(TerminalFrameKind::Hello, 1, 0, { '{', '}' }));
        const auto ping = EncodeTerminalFrame(_frame(TerminalFrameKind::Ping, 2, 0));
        const auto attach = EncodeTerminalFrame(_frame(TerminalFrameKind::Attach, 3, 9));
        std::vector<uint8_t> bytes;
        _append(bytes, hello.Bytes);
        _append(bytes, ping.Bytes);
        _append(bytes, attach.Bytes);

        TerminalFrameDecoder decoder;
        auto decoded = decoder.Feed(bytes);
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::ProducedFrames);
        VERIFY_ARE_EQUAL(size_t{ 3 }, decoded.Frames.size());

        const auto acknowledgement = EncodeAcknowledgementPayload(2);
        const auto duplicate = EncodeTerminalFrame(_frame(
            TerminalFrameKind::Acknowledge,
            3,
            9,
            std::vector<uint8_t>{ acknowledgement.begin(), acknowledgement.end() }));
        decoded = decoder.Feed(duplicate.Bytes);
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::Malformed);
        VERIFY_IS_TRUE(decoder.Failed());
    }

    void TerminalFrameTests::ResizePayloadEnforcesDimensionsAndFrameShape()
    {
        const auto payload = EncodeResizePayload(120, 40);
        auto frame = _frame(
            TerminalFrameKind::Resize,
            1,
            2,
            std::vector<uint8_t>{ payload.begin(), payload.end() });
        std::string error;
        VERIFY_IS_TRUE(ValidateTerminalFrame(frame, MaximumTerminalFrameBytes, error));
        uint32_t columns{};
        uint32_t rows{};
        VERIFY_IS_TRUE(DecodeResizePayload(frame.Payload, columns, rows));
        VERIFY_ARE_EQUAL(uint32_t{ 120 }, columns);
        VERIFY_ARE_EQUAL(uint32_t{ 40 }, rows);

        const auto acknowledgement = EncodeAcknowledgementPayload(42);
        uint64_t acknowledgedSequence{};
        VERIFY_IS_TRUE(DecodeAcknowledgementPayload(acknowledgement, acknowledgedSequence));
        VERIFY_ARE_EQUAL(uint64_t{ 42 }, acknowledgedSequence);
        const std::array<uint8_t, 8> zeroAcknowledgement{};
        VERIFY_IS_FALSE(DecodeAcknowledgementPayload(zeroAcknowledgement, acknowledgedSequence));

        frame.Payload.assign(8, 0);
        VERIFY_IS_FALSE(ValidateTerminalFrame(frame, MaximumTerminalFrameBytes, error));
        frame.Payload.assign(7, 1);
        VERIFY_IS_FALSE(ValidateTerminalFrame(frame, MaximumTerminalFrameBytes, error));

        const auto excessive = EncodeResizePayload(MaximumTerminalDimension + 1, 40);
        frame.Payload.assign(excessive.begin(), excessive.end());
        VERIFY_IS_FALSE(ValidateTerminalFrame(frame, MaximumTerminalFrameBytes, error));
    }

    void TerminalFrameTests::RejectsInvalidMagicVersionKindFlagsAndGeneration()
    {
        const auto valid = EncodeTerminalFrame(_frame(TerminalFrameKind::Hello, 1, 0, { '{', '}' }));
        auto bytes = valid.Bytes;
        bytes[0] = 'X';
        TerminalFrameDecoder decoder;
        VERIFY_IS_TRUE(decoder.Feed(bytes).Status == FrameDecodeStatus::Malformed);

        bytes = valid.Bytes;
        bytes[4] = 2;
        decoder.Reset();
        VERIFY_IS_TRUE(decoder.Feed(bytes).Status == FrameDecodeStatus::Malformed);

        bytes = valid.Bytes;
        bytes[6] = 99;
        decoder.Reset();
        VERIFY_IS_TRUE(decoder.Feed(bytes).Status == FrameDecodeStatus::Malformed);

        std::string error;
        auto input = _frame(TerminalFrameKind::Input, 1, 3, { 'x' });
        input.Flags = static_cast<uint32_t>(TerminalFrameFlag::ResetBeforePayload);
        VERIFY_IS_FALSE(ValidateTerminalFrame(input, MaximumTerminalFrameBytes, error));
        input.Flags = 0;
        input.Generation = 0;
        VERIFY_IS_FALSE(ValidateTerminalFrame(input, MaximumTerminalFrameBytes, error));
        input.Generation = 3;
        input.Sequence = 0;
        VERIFY_IS_FALSE(ValidateTerminalFrame(input, MaximumTerminalFrameBytes, error));
    }

    void TerminalFrameTests::RejectsOversizedFeedsPayloadsAndFrameBursts()
    {
        const std::vector<uint8_t> oversizedFeed(MaximumTransportFeedBytes + 1, 0);
        TerminalFrameDecoder decoder;
        VERIFY_IS_TRUE(decoder.Feed(oversizedFeed).Status == FrameDecodeStatus::ResourceLimit);

        auto header = EncodeTerminalFrame(_frame(TerminalFrameKind::Output, 1, 1, { 'x' })).Bytes;
        header.resize(TerminalFrameHeaderBytes);
        header[12] = 0x01;
        header[13] = 0x01;
        header[14] = 0x00;
        header[15] = 0x00;
        TerminalFrameDecoder smallDecoder{ 256 };
        VERIFY_IS_TRUE(smallDecoder.Feed(header).Status == FrameDecodeStatus::Oversized);

        std::vector<uint8_t> frames;
        for (size_t index = 0; index <= MaximumFramesPerFeed; ++index)
        {
            const auto frame = EncodeTerminalFrame(_frame(TerminalFrameKind::Ping, index + 1, 0));
            _append(frames, frame.Bytes);
        }
        TerminalFrameDecoder bounded;
        VERIFY_IS_TRUE(bounded.Feed(frames).Status == FrameDecodeStatus::ResourceLimit);
    }

    void TerminalFrameTests::FailedDecoderCanBeExplicitlyReset()
    {
        const std::array<uint8_t, TerminalFrameHeaderBytes> invalid{};
        TerminalFrameDecoder decoder;
        VERIFY_IS_TRUE(decoder.Feed(invalid).Failed());
        VERIFY_IS_TRUE(decoder.Failed());

        decoder.Reset();
        const auto ping = EncodeTerminalFrame(_frame(TerminalFrameKind::Ping, 1, 0));
        const auto decoded = decoder.Feed(ping.Bytes);
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::ProducedFrames);
        VERIFY_IS_FALSE(decoder.Failed());
    }
}
