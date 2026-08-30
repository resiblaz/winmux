// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ControlFrame.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        constexpr size_t ControlFrameHeaderBytes{ 4 };

        uint32_t _readUint32(const uint8_t* value) noexcept
        {
            return static_cast<uint32_t>(value[0]) |
                   (static_cast<uint32_t>(value[1]) << 8) |
                   (static_cast<uint32_t>(value[2]) << 16) |
                   (static_cast<uint32_t>(value[3]) << 24);
        }

        void _writeUint32(std::vector<uint8_t>& bytes, const uint32_t value)
        {
            bytes.push_back(static_cast<uint8_t>(value));
            bytes.push_back(static_cast<uint8_t>(value >> 8));
            bytes.push_back(static_cast<uint8_t>(value >> 16));
            bytes.push_back(static_cast<uint8_t>(value >> 24));
        }

        ControlFrameDecodeResult _failure(const FrameDecodeStatus status, std::string error)
        {
            ControlFrameDecodeResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }
    }

    ControlFrameDecoder::ControlFrameDecoder(const uint32_t maximumPayloadBytes) :
        _maximumPayloadBytes{ maximumPayloadBytes }
    {
    }

    ControlFrameDecodeResult ControlFrameDecoder::Feed(const std::span<const uint8_t> bytes)
    {
        if (_failed)
        {
            return _failure(FrameDecodeStatus::Malformed, "Control frame decoder is in a failed state");
        }
        if (_maximumPayloadBytes < MinimumControlFrameBytes ||
            _maximumPayloadBytes > MaximumControlFrameBytes ||
            bytes.size() > MaximumTransportFeedBytes)
        {
            _failed = true;
            return _failure(FrameDecodeStatus::ResourceLimit, "Control frame decoder bounds are invalid or exceeded");
        }
        _buffer.insert(_buffer.end(), bytes.begin(), bytes.end());

        ControlFrameDecodeResult result;
        size_t offset{};
        while (_buffer.size() - offset >= ControlFrameHeaderBytes)
        {
            const auto payloadBytes = _readUint32(_buffer.data() + offset);
            if (payloadBytes == 0)
            {
                _failed = true;
                return _failure(FrameDecodeStatus::Malformed, "Control frame payload cannot be empty");
            }
            if (payloadBytes > _maximumPayloadBytes)
            {
                _failed = true;
                return _failure(FrameDecodeStatus::Oversized, "Control frame payload exceeds the negotiated limit");
            }
            const auto frameBytes = ControlFrameHeaderBytes + static_cast<size_t>(payloadBytes);
            if (_buffer.size() - offset < frameBytes)
            {
                break;
            }
            if (result.Payloads.size() >= MaximumFramesPerFeed)
            {
                _failed = true;
                return _failure(FrameDecodeStatus::ResourceLimit, "Too many control frames arrived in one transport feed");
            }
            const std::span payload{
                _buffer.data() + offset + ControlFrameHeaderBytes,
                static_cast<size_t>(payloadBytes)
            };
            if (!IsValidUtf8(payload))
            {
                _failed = true;
                return _failure(FrameDecodeStatus::Malformed, "Control frame payload is not valid UTF-8");
            }
            result.Payloads.emplace_back(
                reinterpret_cast<const char*>(payload.data()),
                payload.size());
            offset += frameBytes;
        }

        if (offset != 0)
        {
            _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        if (_buffer.size() > ControlFrameHeaderBytes + _maximumPayloadBytes)
        {
            _failed = true;
            return _failure(FrameDecodeStatus::ResourceLimit, "Control frame buffer exceeds its bound");
        }
        result.Status = result.Payloads.empty() ?
                            FrameDecodeStatus::NeedMoreData :
                            FrameDecodeStatus::ProducedFrames;
        return result;
    }

    bool ControlFrameDecoder::Failed() const noexcept
    {
        return _failed;
    }

    void ControlFrameDecoder::Reset() noexcept
    {
        _buffer.clear();
        _failed = false;
    }

    ControlFrameEncodeResult EncodeControlFrame(
        const std::string_view payload,
        const uint32_t maximumPayloadBytes)
    {
        ControlFrameEncodeResult result;
        if (maximumPayloadBytes < MinimumControlFrameBytes ||
            maximumPayloadBytes > MaximumControlFrameBytes)
        {
            result.Error = "Control frame payload limit is invalid";
            return result;
        }
        if (payload.empty() || payload.size() > maximumPayloadBytes)
        {
            result.Error = "Control frame payload is empty or oversized";
            return result;
        }
        if (!IsValidUtf8(payload))
        {
            result.Error = "Control frame payload is not valid UTF-8";
            return result;
        }
        result.Bytes.reserve(ControlFrameHeaderBytes + payload.size());
        _writeUint32(result.Bytes, static_cast<uint32_t>(payload.size()));
        result.Bytes.insert(
            result.Bytes.end(),
            reinterpret_cast<const uint8_t*>(payload.data()),
            reinterpret_cast<const uint8_t*>(payload.data()) + payload.size());
        result.Succeeded = true;
        return result;
    }
}
