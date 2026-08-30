// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalOutputRing.h"

#include <deque>
#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    RuntimeTerminalOutputRing::RuntimeTerminalOutputRing(
        RuntimeTerminalOutputRingConfiguration configuration) noexcept :
        _configuration{ std::move(configuration) }
    {
        _valid = _configuration.Generation != 0 &&
                 _configuration.MaximumBytes != 0 &&
                 _configuration.MaximumFrames != 0 &&
                 _configuration.MaximumFrameBytes != 0 &&
                 _configuration.MaximumFrameBytes <= MaximumTerminalFrameBytes;
    }

    bool RuntimeTerminalOutputRing::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeTerminalOutputAppendResult RuntimeTerminalOutputRing::Append(
        TerminalFrame frame)
    {
        RuntimeTerminalOutputAppendResult result;
        if (!_valid)
        {
            result.Status = RuntimeTerminalOutputAppendStatus::InvalidConfiguration;
            return result;
        }
        if (frame.Generation != _configuration.Generation)
        {
            result.Status = RuntimeTerminalOutputAppendStatus::StaleGeneration;
            return result;
        }
        if (frame.Sequence <= _latestSequence)
        {
            result.Status = RuntimeTerminalOutputAppendStatus::NonMonotonicSequence;
            return result;
        }
        std::string validationError;
        if (frame.Kind != TerminalFrameKind::Output ||
            !ValidateTerminalFrame(
                frame,
                _configuration.MaximumFrameBytes,
                validationError))
        {
            result.Status = RuntimeTerminalOutputAppendStatus::InvalidFrame;
            return result;
        }
        if (frame.Payload.size() > _configuration.MaximumBytes)
        {
            result.Status = RuntimeTerminalOutputAppendStatus::FrameExceedsRing;
            return result;
        }

        const auto frameBytes = frame.Payload.size();
        if (frameBytes > (std::numeric_limits<size_t>::max)() - _retainedBytes)
        {
            result.Status = RuntimeTerminalOutputAppendStatus::ResourceFailure;
            return result;
        }
        try
        {
            _frames.emplace_back(std::move(frame));
        }
        catch (...)
        {
            result.Status = RuntimeTerminalOutputAppendStatus::ResourceFailure;
            return result;
        }
        _retainedBytes += frameBytes;
        _latestSequence = _frames.back().Sequence;
        while (_frames.size() > _configuration.MaximumFrames ||
               _retainedBytes > _configuration.MaximumBytes)
        {
            const auto& evicted = _frames.front();
            _evictedThroughSequence = evicted.Sequence;
            _retainedBytes -= evicted.Payload.size();
            result.EvictedBytes += evicted.Payload.size();
            ++result.EvictedFrames;
            _frames.pop_front();
        }
        result.Status = result.EvictedFrames == 0 ?
                            RuntimeTerminalOutputAppendStatus::Added :
                            RuntimeTerminalOutputAppendStatus::AddedWithEviction;
        return result;
    }

    RuntimeTerminalCatchUpResult RuntimeTerminalOutputRing::CatchUp(
        const uint64_t generation,
        const uint64_t acknowledgedSequence) const
    {
        RuntimeTerminalCatchUpResult result;
        result.EvictedThroughSequence = _evictedThroughSequence;
        result.LatestSequence = _latestSequence;
        if (!_valid)
        {
            result.Status = RuntimeTerminalCatchUpStatus::InvalidConfiguration;
            return result;
        }
        if (generation != _configuration.Generation)
        {
            result.Status = RuntimeTerminalCatchUpStatus::StaleGeneration;
            return result;
        }
        if (acknowledgedSequence > _latestSequence)
        {
            result.Status = RuntimeTerminalCatchUpStatus::AcknowledgementAhead;
            return result;
        }
        if (acknowledgedSequence < _evictedThroughSequence)
        {
            result.Status = RuntimeTerminalCatchUpStatus::ResetRequired;
            return result;
        }
        if (acknowledgedSequence == _latestSequence)
        {
            result.Status = RuntimeTerminalCatchUpStatus::UpToDate;
            return result;
        }

        try
        {
            result.Frames.reserve(_frames.size());
            for (const auto& frame : _frames)
            {
                if (frame.Sequence > acknowledgedSequence)
                {
                    result.Frames.emplace_back(frame);
                }
            }
        }
        catch (...)
        {
            result.Frames.clear();
            result.Status = RuntimeTerminalCatchUpStatus::ResourceFailure;
            return result;
        }
        result.Status = result.Frames.empty() ?
                            RuntimeTerminalCatchUpStatus::UpToDate :
                            RuntimeTerminalCatchUpStatus::Replay;
        return result;
    }

    uint64_t RuntimeTerminalOutputRing::Generation() const noexcept
    {
        return _configuration.Generation;
    }

    uint64_t RuntimeTerminalOutputRing::LatestSequence() const noexcept
    {
        return _latestSequence;
    }

    uint64_t RuntimeTerminalOutputRing::EvictedThroughSequence() const noexcept
    {
        return _evictedThroughSequence;
    }

    std::optional<uint64_t> RuntimeTerminalOutputRing::OldestRetainedSequence() const noexcept
    {
        return _frames.empty() ?
                   std::nullopt :
                   std::optional<uint64_t>{ _frames.front().Sequence };
    }

    size_t RuntimeTerminalOutputRing::RetainedFrames() const noexcept
    {
        return _frames.size();
    }

    size_t RuntimeTerminalOutputRing::RetainedBytes() const noexcept
    {
        return _retainedBytes;
    }
}
