// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ControlFrame.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        void _append(std::vector<uint8_t>& target, const std::vector<uint8_t>& source)
        {
            target.insert(target.end(), source.begin(), source.end());
        }
    }

    class ControlFrameTests
    {
        TEST_CLASS(ControlFrameTests);

        TEST_METHOD(EncodesLittleEndianLengthAndDecodesFragments);
        TEST_METHOD(DecodesCoalescedFramesWithoutLosingRemainder);
        TEST_METHOD(RejectsEmptyOversizedAndMalformedUtf8Payloads);
        TEST_METHOD(BoundsTransportFeedAndFramesPerFeed);
        TEST_METHOD(ReconfiguresLimitOnlyAtAFrameBoundary);
        TEST_METHOD(FailedDecoderRequiresExplicitReset);
    };

    void ControlFrameTests::EncodesLittleEndianLengthAndDecodesFragments()
    {
        constexpr std::string_view payload{ "{\"type\":\"hello\"}" };
        const auto encoded = EncodeControlFrame(payload);
        VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_ARE_EQUAL(payload.size() + 4, encoded.Bytes.size());
        VERIFY_ARE_EQUAL(static_cast<uint8_t>(payload.size()), encoded.Bytes[0]);
        VERIFY_ARE_EQUAL(uint8_t{ 0 }, encoded.Bytes[1]);

        ControlFrameDecoder decoder;
        auto decoded = decoder.Feed(std::span{ encoded.Bytes }.first(3));
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::NeedMoreData);
        decoded = decoder.Feed(std::span{ encoded.Bytes }.subspan(3, 5));
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::NeedMoreData);
        decoded = decoder.Feed(std::span{ encoded.Bytes }.subspan(8));
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::ProducedFrames);
        VERIFY_ARE_EQUAL(size_t{ 1 }, decoded.Payloads.size());
        VERIFY_ARE_EQUAL(std::string{ payload }, decoded.Payloads[0]);
    }

    void ControlFrameTests::DecodesCoalescedFramesWithoutLosingRemainder()
    {
        const auto first = EncodeControlFrame("{\"id\":1}");
        const auto second = EncodeControlFrame("{\"id\":2}");
        const auto third = EncodeControlFrame("{\"id\":3}");
        std::vector<uint8_t> bytes;
        _append(bytes, first.Bytes);
        _append(bytes, second.Bytes);
        bytes.insert(bytes.end(), third.Bytes.begin(), third.Bytes.begin() + 6);

        ControlFrameDecoder decoder;
        auto decoded = decoder.Feed(bytes);
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::ProducedFrames);
        VERIFY_ARE_EQUAL(size_t{ 2 }, decoded.Payloads.size());
        VERIFY_ARE_EQUAL(std::string{ "{\"id\":1}" }, decoded.Payloads[0]);
        VERIFY_ARE_EQUAL(std::string{ "{\"id\":2}" }, decoded.Payloads[1]);

        decoded = decoder.Feed(std::span{ third.Bytes }.subspan(6));
        VERIFY_ARE_EQUAL(size_t{ 1 }, decoded.Payloads.size());
        VERIFY_ARE_EQUAL(std::string{ "{\"id\":3}" }, decoded.Payloads[0]);
    }

    void ControlFrameTests::RejectsEmptyOversizedAndMalformedUtf8Payloads()
    {
        ControlFrameDecoder decoder;
        const std::array<uint8_t, 4> emptyHeader{};
        auto decoded = decoder.Feed(emptyHeader);
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::Malformed);

        decoder.Reset();
        const std::array<uint8_t, 4> oversizedHeader{ 0x01, 0x01, 0x00, 0x00 };
        ControlFrameDecoder smallDecoder{ 256 };
        decoded = smallDecoder.Feed(oversizedHeader);
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::Oversized);

        const std::array<uint8_t, 6> invalidUtf8{ 0x02, 0x00, 0x00, 0x00, 0xc0, 0x80 };
        ControlFrameDecoder utf8Decoder;
        decoded = utf8Decoder.Feed(invalidUtf8);
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::Malformed);
        VERIFY_IS_FALSE(EncodeControlFrame(std::string_view{ "\xc0\x80", 2 }).Succeeded);
    }

    void ControlFrameTests::BoundsTransportFeedAndFramesPerFeed()
    {
        const std::vector<uint8_t> oversizedFeed(MaximumTransportFeedBytes + 1, 0);
        ControlFrameDecoder decoder;
        VERIFY_IS_TRUE(decoder.Feed(oversizedFeed).Status == FrameDecodeStatus::ResourceLimit);

        const auto frame = EncodeControlFrame("{}");
        std::vector<uint8_t> frames;
        for (size_t index = 0; index <= MaximumFramesPerFeed; ++index)
        {
            _append(frames, frame.Bytes);
        }
        ControlFrameDecoder bounded;
        VERIFY_IS_TRUE(bounded.Feed(frames).Status == FrameDecodeStatus::ResourceLimit);
    }

    void ControlFrameTests::ReconfiguresLimitOnlyAtAFrameBoundary()
    {
        ControlFrameDecoder decoder;
        VERIFY_IS_FALSE(decoder.ReconfigureMaximumPayloadBytes(MinimumControlFrameBytes - 1));
        VERIFY_IS_FALSE(decoder.ReconfigureMaximumPayloadBytes(MaximumControlFrameBytes + 1));
        VERIFY_IS_TRUE(decoder.ReconfigureMaximumPayloadBytes(MinimumControlFrameBytes));

        const std::string oversizedPayload(MinimumControlFrameBytes + 1, 'x');
        const auto oversizedFrame = EncodeControlFrame(oversizedPayload);
        VERIFY_IS_TRUE(oversizedFrame.Succeeded);
        VERIFY_IS_TRUE(decoder.Feed(oversizedFrame.Bytes).Status == FrameDecodeStatus::Oversized);
        VERIFY_IS_FALSE(decoder.ReconfigureMaximumPayloadBytes(MaximumControlFrameBytes));

        decoder.Reset();
        const auto validFrame = EncodeControlFrame("{}");
        VERIFY_IS_TRUE(decoder.Feed(std::span{ validFrame.Bytes }.first(3)).Status == FrameDecodeStatus::NeedMoreData);
        VERIFY_IS_FALSE(decoder.ReconfigureMaximumPayloadBytes(MaximumControlFrameBytes));
        VERIFY_IS_TRUE(decoder.Feed(std::span{ validFrame.Bytes }.subspan(3)).Status == FrameDecodeStatus::ProducedFrames);
        VERIFY_IS_TRUE(decoder.ReconfigureMaximumPayloadBytes(MaximumControlFrameBytes));
    }

    void ControlFrameTests::FailedDecoderRequiresExplicitReset()
    {
        ControlFrameDecoder decoder;
        const std::array<uint8_t, 4> invalid{};
        const auto valid = EncodeControlFrame("{}");
        VERIFY_IS_TRUE(decoder.Feed(invalid).Failed());
        VERIFY_IS_TRUE(decoder.Failed());
        VERIFY_IS_TRUE(decoder.Feed(valid.Bytes).Failed());

        decoder.Reset();
        VERIFY_IS_FALSE(decoder.Failed());
        const auto decoded = decoder.Feed(valid.Bytes);
        VERIFY_IS_TRUE(decoded.Status == FrameDecodeStatus::ProducedFrames);
    }
}
