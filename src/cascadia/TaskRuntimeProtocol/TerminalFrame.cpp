// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalFrame.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        uint16_t _readUint16(const uint8_t* value) noexcept
        {
            return static_cast<uint16_t>(value[0]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(value[1]) << 8);
        }

        uint32_t _readUint32(const uint8_t* value) noexcept
        {
            return static_cast<uint32_t>(value[0]) |
                   (static_cast<uint32_t>(value[1]) << 8) |
                   (static_cast<uint32_t>(value[2]) << 16) |
                   (static_cast<uint32_t>(value[3]) << 24);
        }

        uint64_t _readUint64(const uint8_t* value) noexcept
        {
            uint64_t result{};
            for (size_t index = 0; index < 8; ++index)
            {
                result |= static_cast<uint64_t>(value[index]) << (index * 8);
            }
            return result;
        }

        void _appendUint16(std::vector<uint8_t>& bytes, const uint16_t value)
        {
            bytes.push_back(static_cast<uint8_t>(value));
            bytes.push_back(static_cast<uint8_t>(value >> 8));
        }

        void _appendUint32(std::vector<uint8_t>& bytes, const uint32_t value)
        {
            for (size_t index = 0; index < 4; ++index)
            {
                bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
            }
        }

        void _appendUint64(std::vector<uint8_t>& bytes, const uint64_t value)
        {
            for (size_t index = 0; index < 8; ++index)
            {
                bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
            }
        }

        bool _knownKind(const TerminalFrameKind kind) noexcept
        {
            return kind >= TerminalFrameKind::Hello && kind <= TerminalFrameKind::Error;
        }

        bool _connectionLevelKind(const TerminalFrameKind kind) noexcept
        {
            return kind == TerminalFrameKind::Hello ||
                   kind == TerminalFrameKind::Ping ||
                   kind == TerminalFrameKind::Pong;
        }

        TerminalFrameDecodeResult _failure(const FrameDecodeStatus status, std::string error)
        {
            TerminalFrameDecodeResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }
    }

    bool ValidateTerminalFrame(
        const TerminalFrame& frame,
        const uint32_t maximumPayloadBytes,
        std::string& error)
    {
        if (maximumPayloadBytes == 0 || maximumPayloadBytes > MaximumTerminalFrameBytes ||
            frame.Payload.size() > maximumPayloadBytes)
        {
            error = "Terminal frame payload exceeds the negotiated limit";
            return false;
        }
        if (!_knownKind(frame.Kind) || frame.Sequence == 0)
        {
            error = "Terminal frame kind or sequence is invalid";
            return false;
        }
        if (frame.Kind != TerminalFrameKind::Error &&
            ((_connectionLevelKind(frame.Kind) && frame.Generation != 0) ||
             (!_connectionLevelKind(frame.Kind) && frame.Generation == 0)))
        {
            error = "Terminal frame generation is invalid for its kind";
            return false;
        }

        constexpr auto resetFlag = static_cast<uint32_t>(TerminalFrameFlag::ResetBeforePayload);
        constexpr auto endFlag = static_cast<uint32_t>(TerminalFrameFlag::EndOfStream);
        const auto outputFlags = resetFlag | endFlag;
        switch (frame.Kind)
        {
        case TerminalFrameKind::Hello:
            if (frame.Flags != 0 || frame.Payload.empty() ||
                frame.Payload.size() > MaximumTerminalMetadataBytes || !IsValidUtf8(frame.Payload))
            {
                error = "Terminal Hello frame is invalid";
                return false;
            }
            break;
        case TerminalFrameKind::Attach:
            if (frame.Flags != 0 || frame.Payload.size() > MaximumTerminalMetadataBytes ||
                (!frame.Payload.empty() && !IsValidUtf8(frame.Payload)))
            {
                error = "Terminal Attach frame is invalid";
                return false;
            }
            break;
        case TerminalFrameKind::Reset:
            if (frame.Flags != 0 || !frame.Payload.empty())
            {
                error = "Terminal reset frame is invalid";
                return false;
            }
            break;
        case TerminalFrameKind::Acknowledge:
        {
            uint64_t acknowledgedSequence{};
            if (frame.Flags != 0 ||
                !DecodeAcknowledgementPayload(frame.Payload, acknowledgedSequence))
            {
                error = "Terminal acknowledgement frame is invalid";
                return false;
            }
            break;
        }
        case TerminalFrameKind::Output:
            if ((frame.Flags & ~outputFlags) != 0 ||
                (frame.Payload.empty() && (frame.Flags & endFlag) == 0))
            {
                error = "Terminal output frame is invalid";
                return false;
            }
            break;
        case TerminalFrameKind::Input:
            if (frame.Flags != 0 || frame.Payload.empty() ||
                frame.Payload.size() > MaximumTerminalInputBytes)
            {
                error = "Terminal input frame is invalid";
                return false;
            }
            break;
        case TerminalFrameKind::Resize:
        {
            uint32_t columns{};
            uint32_t rows{};
            if (frame.Flags != 0 || !DecodeResizePayload(frame.Payload, columns, rows))
            {
                error = "Terminal resize frame is invalid";
                return false;
            }
            break;
        }
        case TerminalFrameKind::Ping:
        case TerminalFrameKind::Pong:
            if (frame.Flags != 0 || !frame.Payload.empty())
            {
                error = "Terminal heartbeat frame is invalid";
                return false;
            }
            break;
        case TerminalFrameKind::Error:
            if (frame.Flags != 0 || frame.Payload.empty() ||
                frame.Payload.size() > MaximumTerminalMetadataBytes || !IsValidUtf8(frame.Payload))
            {
                error = "Terminal error frame is invalid";
                return false;
            }
            break;
        default:
            error = "Terminal frame kind is unknown";
            return false;
        }
        return true;
    }

    TerminalFrameDecoder::TerminalFrameDecoder(const uint32_t maximumPayloadBytes) :
        _maximumPayloadBytes{ maximumPayloadBytes }
    {
    }

    TerminalFrameDecodeResult TerminalFrameDecoder::Feed(const std::span<const uint8_t> bytes)
    {
        if (_failed)
        {
            return _failure(FrameDecodeStatus::Malformed, "Terminal frame decoder is in a failed state");
        }
        if (_maximumPayloadBytes == 0 || _maximumPayloadBytes > MaximumTerminalFrameBytes ||
            bytes.size() > MaximumTransportFeedBytes)
        {
            _failed = true;
            return _failure(FrameDecodeStatus::ResourceLimit, "Terminal frame decoder bounds are invalid or exceeded");
        }
        _buffer.insert(_buffer.end(), bytes.begin(), bytes.end());

        TerminalFrameDecodeResult result;
        size_t offset{};
        while (_buffer.size() - offset >= TerminalFrameHeaderBytes)
        {
            const auto* header = _buffer.data() + offset;
            if (_readUint32(header) != TerminalFrameMagic ||
                _readUint16(header + 4) != TerminalFrameHeaderVersion)
            {
                _failed = true;
                return _failure(FrameDecodeStatus::Malformed, "Terminal frame magic or header version is invalid");
            }
            const auto kind = static_cast<TerminalFrameKind>(_readUint16(header + 6));
            const auto flags = _readUint32(header + 8);
            const auto payloadBytes = _readUint32(header + 12);
            const auto sequence = _readUint64(header + 16);
            const auto generation = _readUint64(header + 24);
            if (payloadBytes > _maximumPayloadBytes)
            {
                _failed = true;
                return _failure(FrameDecodeStatus::Oversized, "Terminal frame payload exceeds the negotiated limit");
            }
            const auto frameBytes = TerminalFrameHeaderBytes + static_cast<size_t>(payloadBytes);
            if (_buffer.size() - offset < frameBytes)
            {
                break;
            }
            if (result.Frames.size() >= MaximumFramesPerFeed)
            {
                _failed = true;
                return _failure(FrameDecodeStatus::ResourceLimit, "Too many terminal frames arrived in one transport feed");
            }
            if (sequence <= _lastSequence)
            {
                _failed = true;
                return _failure(FrameDecodeStatus::Malformed, "Terminal frame sequence is not strictly increasing");
            }

            TerminalFrame frame;
            frame.Kind = kind;
            frame.Flags = flags;
            frame.Sequence = sequence;
            frame.Generation = generation;
            frame.Payload.assign(header + TerminalFrameHeaderBytes, header + frameBytes);
            std::string validationError;
            if (!ValidateTerminalFrame(frame, _maximumPayloadBytes, validationError))
            {
                _failed = true;
                return _failure(FrameDecodeStatus::Malformed, std::move(validationError));
            }
            result.Frames.emplace_back(std::move(frame));
            _lastSequence = sequence;
            offset += frameBytes;
        }

        if (offset != 0)
        {
            _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        if (_buffer.size() > TerminalFrameHeaderBytes + _maximumPayloadBytes)
        {
            _failed = true;
            return _failure(FrameDecodeStatus::ResourceLimit, "Terminal frame buffer exceeds its bound");
        }
        result.Status = result.Frames.empty() ?
                            FrameDecodeStatus::NeedMoreData :
                            FrameDecodeStatus::ProducedFrames;
        return result;
    }

    bool TerminalFrameDecoder::Failed() const noexcept
    {
        return _failed;
    }

    void TerminalFrameDecoder::Reset() noexcept
    {
        _buffer.clear();
        _lastSequence = 0;
        _failed = false;
    }

    TerminalFrameEncodeResult EncodeTerminalFrame(
        const TerminalFrame& frame,
        const uint32_t maximumPayloadBytes)
    {
        TerminalFrameEncodeResult result;
        if (!ValidateTerminalFrame(frame, maximumPayloadBytes, result.Error))
        {
            return result;
        }
        result.Bytes.reserve(TerminalFrameHeaderBytes + frame.Payload.size());
        _appendUint32(result.Bytes, TerminalFrameMagic);
        _appendUint16(result.Bytes, TerminalFrameHeaderVersion);
        _appendUint16(result.Bytes, static_cast<uint16_t>(frame.Kind));
        _appendUint32(result.Bytes, frame.Flags);
        _appendUint32(result.Bytes, static_cast<uint32_t>(frame.Payload.size()));
        _appendUint64(result.Bytes, frame.Sequence);
        _appendUint64(result.Bytes, frame.Generation);
        result.Bytes.insert(result.Bytes.end(), frame.Payload.begin(), frame.Payload.end());
        result.Succeeded = true;
        return result;
    }

    std::array<uint8_t, 8> EncodeResizePayload(const uint32_t columns, const uint32_t rows) noexcept
    {
        return {
            static_cast<uint8_t>(columns),
            static_cast<uint8_t>(columns >> 8),
            static_cast<uint8_t>(columns >> 16),
            static_cast<uint8_t>(columns >> 24),
            static_cast<uint8_t>(rows),
            static_cast<uint8_t>(rows >> 8),
            static_cast<uint8_t>(rows >> 16),
            static_cast<uint8_t>(rows >> 24),
        };
    }

    bool DecodeResizePayload(
        const std::span<const uint8_t> payload,
        uint32_t& columns,
        uint32_t& rows) noexcept
    {
        columns = 0;
        rows = 0;
        if (payload.size() != 8)
        {
            return false;
        }
        columns = _readUint32(payload.data());
        rows = _readUint32(payload.data() + 4);
        return columns != 0 && columns <= MaximumTerminalDimension &&
               rows != 0 && rows <= MaximumTerminalDimension;
    }

    std::array<uint8_t, 8> EncodeAcknowledgementPayload(const uint64_t sequence) noexcept
    {
        std::array<uint8_t, 8> payload{};
        for (size_t index = 0; index < payload.size(); ++index)
        {
            payload[index] = static_cast<uint8_t>(sequence >> (index * 8));
        }
        return payload;
    }

    bool DecodeAcknowledgementPayload(
        const std::span<const uint8_t> payload,
        uint64_t& sequence) noexcept
    {
        sequence = 0;
        if (payload.size() != 8)
        {
            return false;
        }
        sequence = _readUint64(payload.data());
        return sequence != 0;
    }
}
