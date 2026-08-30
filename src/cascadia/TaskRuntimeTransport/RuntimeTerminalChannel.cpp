// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalChannel.h"

#include "RuntimePipeIo.h"

#include <deque>
#include <mutex>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        struct HandleOwner
        {
            HANDLE Value{ INVALID_HANDLE_VALUE };

            ~HandleOwner()
            {
                if (Value != INVALID_HANDLE_VALUE && Value != nullptr)
                {
                    CloseHandle(Value);
                }
            }
        };

        bool _validTimeout(const uint32_t timeoutMilliseconds) noexcept
        {
            return timeoutMilliseconds != 0 &&
                   timeoutMilliseconds <= MaximumRuntimeTransportTimeoutMilliseconds;
        }

        bool _validMaximumPayload(const uint32_t maximumPayloadBytes) noexcept
        {
            return maximumPayloadBytes != 0 &&
                   maximumPayloadBytes <= MaximumTerminalFrameBytes;
        }

        bool _validGeneration(
            const TerminalFrame& frame,
            const uint64_t expectedGeneration) noexcept
        {
            switch (frame.Kind)
            {
            case TerminalFrameKind::Hello:
            case TerminalFrameKind::Ping:
            case TerminalFrameKind::Pong:
                return frame.Generation == 0;
            case TerminalFrameKind::Error:
                return frame.Generation == 0 ||
                       frame.Generation == expectedGeneration;
            default:
                return frame.Generation == expectedGeneration;
            }
        }
    }

    struct RuntimeTerminalChannel::Impl
    {
        HandleOwner Pipe;
        RuntimeTerminalEndpoint TerminalEndpoint;
        uint32_t MaximumPayload{};
        TerminalFrameDecoder Decoder;
        std::deque<TerminalFrame> PendingFrames;
        std::mutex ReadMutex;
        std::mutex WriteMutex;
        std::mutex IoStartMutex;
        std::atomic_uint64_t CancellationGeneration{};
        std::atomic<RuntimeTransportStatus> TerminalStatus{
            RuntimeTransportStatus::Succeeded
        };
        uint64_t LastSentSequence{};

        Impl(
            const HANDLE handle,
            RuntimeTerminalEndpoint endpoint,
            const uint32_t maximumPayloadBytes) :
            Pipe{ handle },
            TerminalEndpoint{ std::move(endpoint) },
            MaximumPayload{ maximumPayloadBytes },
            Decoder{ maximumPayloadBytes }
        {
        }

        void MarkTerminal(const RuntimeTransportStatus status) noexcept
        {
            auto expected = RuntimeTransportStatus::Succeeded;
            TerminalStatus.compare_exchange_strong(
                expected,
                status,
                std::memory_order_acq_rel);
        }
    };

    RuntimeTerminalChannel::RuntimeTerminalChannel() noexcept = default;
    RuntimeTerminalChannel::~RuntimeTerminalChannel() = default;
    RuntimeTerminalChannel::RuntimeTerminalChannel(RuntimeTerminalChannel&&) noexcept = default;
    RuntimeTerminalChannel& RuntimeTerminalChannel::operator=(RuntimeTerminalChannel&&) noexcept = default;

    RuntimeTerminalChannel::RuntimeTerminalChannel(
        const HANDLE handle,
        RuntimeTerminalEndpoint endpoint,
        const uint32_t maximumPayloadBytes) :
        _impl{ std::make_unique<Impl>(
            handle,
            std::move(endpoint),
            maximumPayloadBytes) }
    {
    }

    bool RuntimeTerminalChannel::Connected() const noexcept
    {
        return _impl && _impl->Pipe.Value != INVALID_HANDLE_VALUE &&
               _impl->TerminalStatus.load(std::memory_order_acquire) ==
                   RuntimeTransportStatus::Succeeded;
    }

    RuntimeTerminalChannel::operator bool() const noexcept
    {
        return Connected();
    }

    RuntimeTransportResult RuntimeTerminalChannel::Send(
        const TerminalFrame& frame,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        if (!_impl || _impl->Pipe.Value == INVALID_HANDLE_VALUE ||
            !_validTimeout(timeoutMilliseconds) ||
            !_validGeneration(frame, _impl->TerminalEndpoint.Generation))
        {
            return details::RuntimeTransportFailure(
                RuntimeTransportStatus::InvalidConfiguration,
                "Terminal channel, timeout, or frame generation is invalid");
        }
        const auto cancellationGeneration =
            _impl->CancellationGeneration.load(std::memory_order_acquire);
        const auto terminalStatus =
            _impl->TerminalStatus.load(std::memory_order_acquire);
        if (terminalStatus != RuntimeTransportStatus::Succeeded)
        {
            return details::RuntimeTransportFailure(
                terminalStatus,
                "Terminal channel connection epoch is terminal");
        }
        if (const auto cancelled = details::RuntimeCancellationFailure(
                cancellationEvent))
        {
            return *cancelled;
        }
        const auto encoded = EncodeTerminalFrame(
            frame,
            _impl->MaximumPayload);
        if (!encoded.Succeeded)
        {
            return details::RuntimeTransportFailure(
                RuntimeTransportStatus::InvalidConfiguration,
                encoded.Error);
        }

        std::scoped_lock lock{ _impl->WriteMutex };
        if (frame.Sequence <= _impl->LastSentSequence)
        {
            return details::RuntimeTransportFailure(
                RuntimeTransportStatus::InvalidConfiguration,
                "Terminal send sequence is not strictly increasing");
        }
        auto sent = details::WriteRuntimePipe(
            _impl->Pipe.Value,
            encoded.Bytes,
            timeoutMilliseconds,
            cancellationEvent,
            _impl->IoStartMutex,
            _impl->CancellationGeneration,
            cancellationGeneration);
        if (!sent.Succeeded())
        {
            _impl->MarkTerminal(sent.Status);
            CancelPendingIo();
            return sent;
        }
        _impl->LastSentSequence = frame.Sequence;
        return sent;
    }

    RuntimeTerminalReceiveResult RuntimeTerminalChannel::Receive(
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        RuntimeTerminalReceiveResult result;
        if (!_impl || _impl->Pipe.Value == INVALID_HANDLE_VALUE ||
            !_validTimeout(timeoutMilliseconds))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Terminal channel or timeout is invalid";
            return result;
        }
        const auto cancellationGeneration =
            _impl->CancellationGeneration.load(std::memory_order_acquire);
        const auto terminalStatus =
            _impl->TerminalStatus.load(std::memory_order_acquire);
        if (terminalStatus != RuntimeTransportStatus::Succeeded)
        {
            static_cast<RuntimeTransportResult&>(result) =
                details::RuntimeTransportFailure(
                    terminalStatus,
                    "Terminal channel connection epoch is terminal");
            return result;
        }

        std::scoped_lock lock{ _impl->ReadMutex };
        if (const auto cancelled = details::RuntimeCancellationFailure(
                cancellationEvent))
        {
            static_cast<RuntimeTransportResult&>(result) = *cancelled;
            return result;
        }
        if (const auto cancelled = details::RuntimeGenerationCancellationFailure(
                _impl->CancellationGeneration,
                cancellationGeneration))
        {
            static_cast<RuntimeTransportResult&>(result) = *cancelled;
            return result;
        }
        if (_impl->Decoder.Failed())
        {
            result.Status = RuntimeTransportStatus::MalformedFrame;
            result.Error = "Terminal frame decoder is in a failed state";
            _impl->MarkTerminal(result.Status);
            CancelPendingIo();
            return result;
        }
        if (!_impl->PendingFrames.empty())
        {
            result.Status = RuntimeTransportStatus::Succeeded;
            result.Frame = std::move(_impl->PendingFrames.front());
            _impl->PendingFrames.pop_front();
            return result;
        }

        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        std::array<uint8_t, MaximumTransportFeedBytes> buffer{};
        for (;;)
        {
            const auto now = GetTickCount64();
            if (now >= deadline)
            {
                result.Status = RuntimeTransportStatus::Timeout;
                result.Error = "Terminal frame receive timed out";
                return result;
            }
            DWORD transferred{};
            const auto read = details::ReadRuntimePipe(
                _impl->Pipe.Value,
                buffer,
                static_cast<uint32_t>(deadline - now),
                transferred,
                cancellationEvent,
                _impl->IoStartMutex,
                _impl->CancellationGeneration,
                cancellationGeneration);
            if (!read.Succeeded())
            {
                static_cast<RuntimeTransportResult&>(result) = read;
                if (read.Status != RuntimeTransportStatus::Timeout &&
                    read.Status != RuntimeTransportStatus::Cancelled)
                {
                    _impl->MarkTerminal(read.Status);
                    CancelPendingIo();
                }
                return result;
            }
            if (transferred == 0)
            {
                result.Status = RuntimeTransportStatus::Disconnected;
                result.Error = "Terminal pipe disconnected";
                _impl->MarkTerminal(result.Status);
                CancelPendingIo();
                return result;
            }

            auto decoded = _impl->Decoder.Feed(
                std::span{ buffer }.first(transferred));
            if (decoded.Failed() ||
                std::ranges::any_of(decoded.Frames, [&](const auto& frame) {
                    return !_validGeneration(
                        frame,
                        _impl->TerminalEndpoint.Generation);
                }))
            {
                result.Status = RuntimeTransportStatus::MalformedFrame;
                result.Error = decoded.Failed() ?
                                   std::move(decoded.Error) :
                                   "Terminal frame generation does not match the endpoint";
                _impl->MarkTerminal(result.Status);
                CancelPendingIo();
                return result;
            }
            for (auto& frame : decoded.Frames)
            {
                _impl->PendingFrames.emplace_back(std::move(frame));
            }
            if (!_impl->PendingFrames.empty())
            {
                result.Status = RuntimeTransportStatus::Succeeded;
                result.Frame = std::move(_impl->PendingFrames.front());
                _impl->PendingFrames.pop_front();
                return result;
            }
        }
    }

    HANDLE RuntimeTerminalChannel::Handle() const noexcept
    {
        return _impl && _impl->Pipe.Value != INVALID_HANDLE_VALUE ?
                   _impl->Pipe.Value :
                   INVALID_HANDLE_VALUE;
    }

    uint32_t RuntimeTerminalChannel::MaximumPayloadBytes() const noexcept
    {
        return _impl ? _impl->MaximumPayload : 0;
    }

    const RuntimeTerminalEndpoint* RuntimeTerminalChannel::Endpoint() const noexcept
    {
        return _impl ? &_impl->TerminalEndpoint : nullptr;
    }

    void RuntimeTerminalChannel::CancelPendingIo() noexcept
    {
        if (_impl && _impl->Pipe.Value != INVALID_HANDLE_VALUE)
        {
            _impl->MarkTerminal(RuntimeTransportStatus::Cancelled);
            _impl->CancellationGeneration.fetch_add(1, std::memory_order_acq_rel);
            std::scoped_lock startLock{ _impl->IoStartMutex };
            CancelIoEx(_impl->Pipe.Value, nullptr);
        }
    }

    void RuntimeTerminalChannel::Close() noexcept
    {
        _impl.reset();
    }

}
