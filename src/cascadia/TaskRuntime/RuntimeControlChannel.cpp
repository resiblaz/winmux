// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlChannel.h"

#include "ControlFrame.h"
#include "ProtocolJson.h"

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

        RuntimeTransportResult _failure(
            const RuntimeTransportStatus status,
            std::string error,
            const DWORD systemError = ERROR_SUCCESS)
        {
            RuntimeTransportResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        RuntimeTransportStatus _classifyIoError(const DWORD error) noexcept
        {
            return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                           error == ERROR_NO_DATA ?
                       RuntimeTransportStatus::Disconnected :
                       RuntimeTransportStatus::SystemError;
        }

        RuntimeTransportResult _waitOverlapped(
            const HANDLE handle,
            OVERLAPPED& operation,
            const uint32_t timeoutMilliseconds,
            DWORD& transferred)
        {
            const auto wait = WaitForSingleObject(operation.hEvent, timeoutMilliseconds);
            if (wait == WAIT_TIMEOUT)
            {
                CancelIoEx(handle, &operation);
                GetOverlappedResult(handle, &operation, &transferred, TRUE);
                return _failure(RuntimeTransportStatus::Timeout, "Named-pipe operation timed out", WAIT_TIMEOUT);
            }
            if (wait != WAIT_OBJECT_0)
            {
                const auto error = GetLastError();
                CancelIoEx(handle, &operation);
                GetOverlappedResult(handle, &operation, &transferred, TRUE);
                return _failure(RuntimeTransportStatus::SystemError, "Named-pipe wait failed", error);
            }
            if (!GetOverlappedResult(handle, &operation, &transferred, FALSE))
            {
                const auto error = GetLastError();
                return _failure(_classifyIoError(error), "Named-pipe operation failed", error);
            }
            RuntimeTransportResult result;
            result.Status = RuntimeTransportStatus::Succeeded;
            return result;
        }

        RuntimeTransportResult _writeOverlapped(
            const HANDLE handle,
            const std::span<const uint8_t> bytes,
            const uint32_t timeoutMilliseconds)
        {
            size_t offset{};
            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            while (offset < bytes.size())
            {
                HandleOwner event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
                if (!event.Value)
                {
                    return _failure(RuntimeTransportStatus::SystemError, "CreateEvent failed", GetLastError());
                }
                OVERLAPPED operation{};
                operation.hEvent = event.Value;
                DWORD transferred{};
                const auto remainingBytes = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, MAXDWORD));
                const auto started = WriteFile(
                    handle,
                    bytes.data() + offset,
                    remainingBytes,
                    &transferred,
                    &operation);
                if (!started)
                {
                    const auto error = GetLastError();
                    if (error != ERROR_IO_PENDING)
                    {
                        return _failure(_classifyIoError(error), "Named-pipe write failed", error);
                    }
                    const auto now = GetTickCount64();
                    if (now >= deadline)
                    {
                        CancelIoEx(handle, &operation);
                        GetOverlappedResult(handle, &operation, &transferred, TRUE);
                        return _failure(RuntimeTransportStatus::Timeout, "Named-pipe write timed out", WAIT_TIMEOUT);
                    }
                    const auto waited = _waitOverlapped(
                        handle,
                        operation,
                        static_cast<uint32_t>(deadline - now),
                        transferred);
                    if (!waited.Succeeded())
                    {
                        return waited;
                    }
                }
                if (transferred == 0)
                {
                    return _failure(RuntimeTransportStatus::Disconnected, "Named-pipe write completed with zero bytes");
                }
                offset += transferred;
            }
            RuntimeTransportResult result;
            result.Status = RuntimeTransportStatus::Succeeded;
            return result;
        }

        RuntimeTransportResult _readOverlapped(
            const HANDLE handle,
            const std::span<uint8_t> buffer,
            const uint32_t timeoutMilliseconds,
            DWORD& transferred)
        {
            HandleOwner event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            if (!event.Value)
            {
                return _failure(RuntimeTransportStatus::SystemError, "CreateEvent failed", GetLastError());
            }
            OVERLAPPED operation{};
            operation.hEvent = event.Value;
            transferred = 0;
            const auto started = ReadFile(
                handle,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &transferred,
                &operation);
            if (!started)
            {
                const auto error = GetLastError();
                if (error != ERROR_IO_PENDING)
                {
                    return _failure(_classifyIoError(error), "Named-pipe read failed", error);
                }
                return _waitOverlapped(handle, operation, timeoutMilliseconds, transferred);
            }
            RuntimeTransportResult result;
            result.Status = RuntimeTransportStatus::Succeeded;
            return result;
        }

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

        explicit Impl(const HANDLE handle) :
            Pipe{ handle }
        {
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

    RuntimeControlChannel::operator bool() const noexcept
    {
        return _impl && _impl->Pipe.Value != INVALID_HANDLE_VALUE;
    }

    RuntimeTransportResult RuntimeControlChannel::Send(
        const std::string_view payload,
        const uint32_t timeoutMilliseconds)
    {
        if (!*this || !_validTimeout(timeoutMilliseconds))
        {
            return _failure(RuntimeTransportStatus::InvalidConfiguration, "Control channel or timeout is invalid");
        }
        const auto frame = EncodeControlFrame(payload, _impl->MaximumPayloadBytes);
        if (!frame.Succeeded)
        {
            return _failure(RuntimeTransportStatus::InvalidConfiguration, frame.Error);
        }
        std::scoped_lock lock{ _impl->WriteMutex };
        return _writeOverlapped(_impl->Pipe.Value, frame.Bytes, timeoutMilliseconds);
    }

    RuntimeReceiveResult RuntimeControlChannel::Receive(const uint32_t timeoutMilliseconds)
    {
        RuntimeReceiveResult result;
        if (!*this || !_validTimeout(timeoutMilliseconds))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Control channel or timeout is invalid";
            return result;
        }
        std::scoped_lock lock{ _impl->ReadMutex };
        if (_impl->Decoder.Failed())
        {
            result.Status = RuntimeTransportStatus::MalformedFrame;
            result.Error = "Control frame decoder is in a failed state";
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
            const auto read = _readOverlapped(
                _impl->Pipe.Value,
                buffer,
                static_cast<uint32_t>(deadline - now),
                transferred);
            if (!read.Succeeded())
            {
                result.Status = read.Status;
                result.SystemError = read.SystemError;
                result.Error = read.Error;
                return result;
            }
            if (transferred == 0)
            {
                result.Status = RuntimeTransportStatus::Disconnected;
                result.Error = "Control pipe disconnected";
                return result;
            }
            auto decoded = _impl->Decoder.Feed(std::span{ buffer }.first(transferred));
            if (decoded.Failed())
            {
                result.Status = RuntimeTransportStatus::MalformedFrame;
                result.Error = std::move(decoded.Error);
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
        return *this ? _impl->Pipe.Value : INVALID_HANDLE_VALUE;
    }

    void RuntimeControlChannel::Close() noexcept
    {
        _impl.reset();
    }

    RuntimeControlChannelResult AcceptRuntimeControlChannel(
        RuntimeNamedPipe& listener,
        const uint32_t timeoutMilliseconds)
    {
        RuntimeControlChannelResult result;
        if (!listener || !_validTimeout(timeoutMilliseconds))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Control listener or timeout is invalid";
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
                const auto waited = _waitOverlapped(
                    listener.Handle(),
                    operation,
                    timeoutMilliseconds,
                    transferred);
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
                result.Status = _classifyIoError(error);
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

    RuntimeControlChannelResult ConnectRuntimeControlChannel(
        const RuntimeIdentity& identity,
        const uint32_t timeoutMilliseconds)
    {
        RuntimeControlChannelResult result;
        if (identity.ControlPipeName.empty() || !_validTimeout(timeoutMilliseconds))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Runtime identity or timeout is invalid";
            return result;
        }
        if (!WaitNamedPipeW(identity.ControlPipeName.c_str(), timeoutMilliseconds))
        {
            const auto error = GetLastError();
            result.Status = error == ERROR_SEM_TIMEOUT || error == ERROR_FILE_NOT_FOUND ?
                                RuntimeTransportStatus::Timeout :
                                _classifyIoError(error);
            result.SystemError = error;
            result.Error = "WaitNamedPipe failed";
            return result;
        }

        HandleOwner pipe{ CreateFileW(
            identity.ControlPipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
            nullptr) };
        if (pipe.Value == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            result.Status = _classifyIoError(error);
            result.SystemError = error;
            result.Error = "CreateFile for control pipe failed";
            return result;
        }
        DWORD mode{ PIPE_READMODE_BYTE };
        if (!SetNamedPipeHandleState(pipe.Value, &mode, nullptr, nullptr))
        {
            result.SystemError = GetLastError();
            result.Error = "SetNamedPipeHandleState failed";
            return result;
        }

        try
        {
            RuntimeControlChannel channel{ pipe.Value };
            (void)pipe.Release();
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
