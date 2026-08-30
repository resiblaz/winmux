// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlChannel.h"
#include "RuntimeControlIo.h"

#include "ControlFrame.h"

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

            [[nodiscard]] HANDLE Release() noexcept
            {
                return std::exchange(Value, INVALID_HANDLE_VALUE);
            }
        };

        bool _validTimeout(const uint32_t timeoutMilliseconds) noexcept
        {
            return timeoutMilliseconds != 0 &&
                   timeoutMilliseconds <= MaximumRuntimeTransportTimeoutMilliseconds;
        }

    }

    struct RuntimeControlChannel::Impl
    {
        HandleOwner Pipe;
        uint32_t MaximumPayloadBytes{ MaximumControlFrameBytes };
        ControlFrameDecoder Decoder;
        std::deque<std::string> PendingPayloads;
        std::mutex ReadMutex;
        std::mutex WriteMutex;
        std::mutex IoStartMutex;
        std::atomic_uint64_t CancellationGeneration{};
        std::atomic<RuntimeTransportStatus> TerminalStatus{ RuntimeTransportStatus::Succeeded };

        explicit Impl(const HANDLE handle) :
            Pipe{ handle }
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

    RuntimeControlChannel::RuntimeControlChannel() noexcept = default;
    RuntimeControlChannel::~RuntimeControlChannel() = default;
    RuntimeControlChannel::RuntimeControlChannel(RuntimeControlChannel&&) noexcept = default;
    RuntimeControlChannel& RuntimeControlChannel::operator=(RuntimeControlChannel&&) noexcept = default;

    RuntimeControlChannel::RuntimeControlChannel(const HANDLE handle) :
        _impl{ std::make_unique<Impl>(handle) }
    {
    }

    bool RuntimeControlChannel::Connected() const noexcept
    {
        return _impl && _impl->Pipe.Value != INVALID_HANDLE_VALUE &&
               _impl->TerminalStatus.load(std::memory_order_acquire) ==
                   RuntimeTransportStatus::Succeeded;
    }

    RuntimeControlChannel::operator bool() const noexcept
    {
        return Connected();
    }

    RuntimeTransportResult RuntimeControlChannel::Send(
        const std::string_view payload,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        if (!_impl || _impl->Pipe.Value == INVALID_HANDLE_VALUE || !_validTimeout(timeoutMilliseconds))
        {
            return details::RuntimeTransportFailure(
                RuntimeTransportStatus::InvalidConfiguration,
                "Control channel or timeout is invalid");
        }
        const auto cancellationGeneration =
            _impl->CancellationGeneration.load(std::memory_order_acquire);
        const auto terminalStatus = _impl->TerminalStatus.load(std::memory_order_acquire);
        if (terminalStatus != RuntimeTransportStatus::Succeeded)
        {
            return details::RuntimeTransportFailure(
                terminalStatus,
                "Control channel connection epoch is terminal");
        }
        if (const auto cancelled = details::RuntimeCancellationFailure(cancellationEvent))
        {
            return *cancelled;
        }
        const auto frame = EncodeControlFrame(payload, _impl->MaximumPayloadBytes);
        if (!frame.Succeeded)
        {
            return details::RuntimeTransportFailure(
                RuntimeTransportStatus::InvalidConfiguration,
                frame.Error);
        }
        std::scoped_lock lock{ _impl->WriteMutex };
        auto sent = details::WriteRuntimeControl(
            _impl->Pipe.Value,
            frame.Bytes,
            timeoutMilliseconds,
            cancellationEvent,
            _impl->IoStartMutex,
            _impl->CancellationGeneration,
            cancellationGeneration);
        if (!sent.Succeeded())
        {
            _impl->MarkTerminal(sent.Status);
            CancelPendingIo();
        }
        return sent;
    }

    RuntimeReceiveResult RuntimeControlChannel::Receive(
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        RuntimeReceiveResult result;
        if (!_impl || _impl->Pipe.Value == INVALID_HANDLE_VALUE || !_validTimeout(timeoutMilliseconds))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Control channel or timeout is invalid";
            return result;
        }
        const auto cancellationGeneration =
            _impl->CancellationGeneration.load(std::memory_order_acquire);
        const auto terminalStatus = _impl->TerminalStatus.load(std::memory_order_acquire);
        if (terminalStatus != RuntimeTransportStatus::Succeeded)
        {
            static_cast<RuntimeTransportResult&>(result) =
                details::RuntimeTransportFailure(
                    terminalStatus,
                    "Control channel connection epoch is terminal");
            return result;
        }
        std::scoped_lock lock{ _impl->ReadMutex };
        if (const auto cancelled = details::RuntimeCancellationFailure(cancellationEvent))
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
            result.Error = "Control frame decoder is in a failed state";
            _impl->MarkTerminal(result.Status);
            CancelPendingIo();
            return result;
        }
        if (!_impl->PendingPayloads.empty())
        {
            result.Status = RuntimeTransportStatus::Succeeded;
            result.Payload = std::move(_impl->PendingPayloads.front());
            _impl->PendingPayloads.pop_front();
            return result;
        }

        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        std::array<uint8_t, MaximumTransportFeedBytes> buffer{};
        while (true)
        {
            const auto now = GetTickCount64();
            if (now >= deadline)
            {
                result.Status = RuntimeTransportStatus::Timeout;
                result.Error = "Control frame receive timed out";
                return result;
            }
            DWORD transferred{};
            const auto read = details::ReadRuntimeControl(
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
                result.Status = read.Status;
                result.SystemError = read.SystemError;
                result.Error = read.Error;
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
                result.Error = "Control pipe disconnected";
                _impl->MarkTerminal(result.Status);
                CancelPendingIo();
                return result;
            }
            auto decoded = _impl->Decoder.Feed(std::span{ buffer }.first(transferred));
            if (decoded.Failed())
            {
                result.Status = RuntimeTransportStatus::MalformedFrame;
                result.Error = std::move(decoded.Error);
                _impl->MarkTerminal(result.Status);
                CancelPendingIo();
                return result;
            }
            for (auto& payload : decoded.Payloads)
            {
                _impl->PendingPayloads.emplace_back(std::move(payload));
            }
            if (!_impl->PendingPayloads.empty())
            {
                result.Status = RuntimeTransportStatus::Succeeded;
                result.Payload = std::move(_impl->PendingPayloads.front());
                _impl->PendingPayloads.pop_front();
                return result;
            }
        }
    }

    bool RuntimeControlChannel::ConfigureMaximumPayloadBytes(const uint32_t maximumPayloadBytes) noexcept
    {
        if (!*this || !_impl->PendingPayloads.empty() ||
            !_impl->Decoder.ReconfigureMaximumPayloadBytes(maximumPayloadBytes))
        {
            return false;
        }
        _impl->MaximumPayloadBytes = maximumPayloadBytes;
        return true;
    }

    HANDLE RuntimeControlChannel::Handle() const noexcept
    {
        return _impl && _impl->Pipe.Value != INVALID_HANDLE_VALUE ?
                   _impl->Pipe.Value :
                   INVALID_HANDLE_VALUE;
    }

    void RuntimeControlChannel::CancelPendingIo() noexcept
    {
        if (_impl && _impl->Pipe.Value != INVALID_HANDLE_VALUE)
        {
            _impl->MarkTerminal(RuntimeTransportStatus::Cancelled);
            _impl->CancellationGeneration.fetch_add(1, std::memory_order_acq_rel);
            std::scoped_lock startLock{ _impl->IoStartMutex };
            CancelIoEx(_impl->Pipe.Value, nullptr);
        }
    }

    void RuntimeControlChannel::Close() noexcept
    {
        _impl.reset();
    }

    RuntimeControlChannelResult AcceptRuntimeControlChannel(
        RuntimeNamedPipe& listener,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        RuntimeControlChannelResult result;
        if (!listener || !_validTimeout(timeoutMilliseconds))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Control listener or timeout is invalid";
            return result;
        }
        if (const auto cancelled = details::RuntimeCancellationFailure(cancellationEvent))
        {
            static_cast<RuntimeTransportResult&>(result) = *cancelled;
            return result;
        }

        HandleOwner event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        if (!event.Value)
        {
            result.SystemError = GetLastError();
            result.Error = "CreateEvent failed";
            return result;
        }
        OVERLAPPED operation{};
        operation.hEvent = event.Value;
        DWORD transferred{};
        const auto connected = ConnectNamedPipe(listener.Handle(), &operation);
        if (!connected)
        {
            const auto error = GetLastError();
            if (error == ERROR_IO_PENDING)
            {
                const auto waited = details::WaitRuntimeOverlapped(
                    listener.Handle(),
                    operation,
                    timeoutMilliseconds,
                    transferred,
                    cancellationEvent);
                if (!waited.Succeeded())
                {
                    result.Status = waited.Status;
                    result.SystemError = waited.SystemError;
                    result.Error = waited.Error;
                    return result;
                }
            }
            else if (error != ERROR_PIPE_CONNECTED)
            {
                result.Status = details::ClassifyRuntimeIoError(error);
                result.SystemError = error;
                result.Error = "ConnectNamedPipe failed";
                return result;
            }
        }

        try
        {
            RuntimeControlChannel channel{ listener.Handle() };
            (void)listener.Release();
            result.Channel = std::move(channel);
            result.Status = RuntimeTransportStatus::Succeeded;
            return result;
        }
        catch (const std::exception& exception)
        {
            result.Error = exception.what();
            return result;
        }
    }

}
