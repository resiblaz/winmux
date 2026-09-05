// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskRuntimeConnection.h"

#include "ProtocolCapabilities.h"
#include "ProtocolHandshake.h"
#include "RuntimeTerminalEndpoint.h"
#include "RuntimeTerminalHandshake.h"
#include "TerminalFrame.h"

namespace winrt::TerminalApp::implementation::tasks
{
    using namespace winrt::Microsoft::Terminal::TerminalConnection;
    using namespace ::Microsoft::Terminal::TaskRuntime;
    using namespace ::Microsoft::Terminal::TaskRuntimeProtocol;

    TaskRuntimeConnection::TaskRuntimeConnection(
        TaskSurfaceDescriptor descriptor,
        std::function<void(std::wstring, uint64_t)> closeAuthorized) :
        _descriptor{ std::move(descriptor) },
        _closeAuthorized{ std::move(closeAuthorized) },
        _stopEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }
    {
    }

    TaskRuntimeConnection::~TaskRuntimeConnection()
    {
        Close();
    }

    void TaskRuntimeConnection::Start()
    {
        {
            const std::scoped_lock lock{ _stateMutex };
            if (_state != ConnectionState::NotConnected || !_stopEvent)
            {
                return;
            }
        }
        _transition(ConnectionState::Connecting);
        try
        {
            const auto self = get_strong();
            _reader = std::thread{ [self] { self->_run(); } };
        }
        catch (...)
        {
            _transition(ConnectionState::Failed);
        }
    }

    void TaskRuntimeConnection::_run() noexcept
    {
        try
        {
            RuntimeTerminalEndpoint endpoint;
            endpoint.PipeName = _descriptor.PipeName;
            endpoint.RuntimeIdentity = _descriptor.RuntimeIdentity;
            endpoint.TerminalSessionId = _descriptor.TerminalSessionId;
            endpoint.Generation = _descriptor.Generation;
            if (!ValidateRuntimeTerminalEndpoint(endpoint))
            {
                _transition(ConnectionState::Failed);
                return;
            }
            auto connected = ConnectRuntimeTerminalChannel(
                endpoint,
                MaximumTerminalFrameBytes,
                5000,
                _stopEvent.get());
            if (!connected.Succeeded())
            {
                _transition(ConnectionState::Failed);
                return;
            }
            HelloRequest hello;
            hello.ClientInstanceId = _descriptor.ClientInstanceId;
            hello.ClientKind = ProtocolClientKind::TerminalUi;
            hello.ExpectedRuntimeIdentity = _descriptor.RuntimeIdentity;
            hello.RequiredCapabilities = static_cast<uint64_t>(ProtocolCapability::TerminalStream);
            hello.OptionalCapabilities =
                static_cast<uint64_t>(ProtocolCapability::InputControllerLease) |
                static_cast<uint64_t>(ProtocolCapability::TaskTerminalSurfaces);
            auto handshake = PerformRuntimeTerminalClientHandshake(
                connected.Channel,
                hello,
                _descriptor.RuntimeInstanceId,
                5000,
                _stopEvent.get());
            if (!handshake.Succeeded())
            {
                _transition(ConnectionState::Failed);
                return;
            }
            {
                const std::scoped_lock lock{ _writeMutex };
                _channel = std::move(connected.Channel);
                TerminalFrame attach;
                attach.Kind = TerminalFrameKind::Attach;
                attach.Sequence = _nextClientSequence++;
                attach.Generation = _descriptor.Generation;
                const auto acknowledged = std::to_string(_lastOutputSequence);
                attach.Payload.assign(acknowledged.begin(), acknowledged.end());
                if (!_channel.Send(attach, 5000, _stopEvent.get()).Succeeded())
                {
                    _transition(ConnectionState::Failed);
                    return;
                }
            }
            _transition(ConnectionState::Connected);

            for (;;)
            {
                const auto received = _channel.Receive(5000, _stopEvent.get());
                if (received.Status == RuntimeTransportStatus::Timeout)
                {
                    if (WaitForSingleObject(_stopEvent.get(), 0) == WAIT_OBJECT_0)
                    {
                        break;
                    }
                    continue;
                }
                if (!received.Succeeded())
                {
                    if (WaitForSingleObject(_stopEvent.get(), 0) != WAIT_OBJECT_0)
                    {
                        _transition(ConnectionState::Failed);
                    }
                    break;
                }
                const auto& frame = received.Frame;
                if (frame.Kind == TerminalFrameKind::Output)
                {
                    std::wstring output;
                    if ((frame.Flags & static_cast<uint32_t>(TerminalFrameFlag::ResetBeforePayload)) != 0)
                    {
                        output.append(L"\x1b[2J\x1b[H");
                    }
                    if (!frame.Payload.empty())
                    {
                        try
                        {
                            output.append(til::u8u16(std::string_view{
                                reinterpret_cast<const char*>(frame.Payload.data()),
                                frame.Payload.size() }));
                        }
                        catch (...)
                        {
                            output.append(L"\r\n\x1b[31m[invalid Task terminal output]\x1b[0m\r\n");
                        }
                    }
                    _raiseOutput(output);
                    _lastOutputSequence = frame.Sequence;
                    const auto payload = EncodeAcknowledgementPayload(frame.Sequence);
                    static_cast<void>(_send(
                        TerminalFrameKind::Acknowledge,
                        { payload.begin(), payload.end() },
                        _descriptor.Generation));
                    if ((frame.Flags & static_cast<uint32_t>(TerminalFrameFlag::EndOfStream)) != 0)
                    {
                        const auto close = _closeAuthorized;
                        if (close)
                        {
                            close(_descriptor.TaskId, _descriptor.Generation);
                        }
                        _transition(ConnectionState::Closed);
                        break;
                    }
                }
                else if (frame.Kind == TerminalFrameKind::Error)
                {
                    _raiseOutput(L"\r\n\x1b[31m[Task terminal disconnected]\x1b[0m\r\n");
                    _transition(ConnectionState::Failed);
                    break;
                }
            }
        }
        catch (...)
        {
            if (WaitForSingleObject(_stopEvent.get(), 0) != WAIT_OBJECT_0)
            {
                _transition(ConnectionState::Failed);
            }
        }
    }

    bool TaskRuntimeConnection::_send(
        const TerminalFrameKind kind,
        std::vector<uint8_t> payload,
        const uint64_t generation)
    {
        const std::scoped_lock lock{ _writeMutex };
        if (!_channel.Connected())
        {
            return false;
        }
        TerminalFrame frame;
        frame.Kind = kind;
        frame.Sequence = _nextClientSequence++;
        frame.Generation = generation;
        frame.Payload = std::move(payload);
        return _channel.Send(frame, 5000, _stopEvent.get()).Succeeded();
    }

    void TaskRuntimeConnection::WriteInput(const winrt::array_view<const char16_t> data)
    {
        TaskTerminalInputBatch batch;
        {
            const std::scoped_lock lock{ _inputMutex };
            batch = _input.Feed({ reinterpret_cast<const wchar_t*>(data.data()), data.size() });
        }
        _raiseOutput(batch.Echo);
        if (batch.Interrupt)
        {
            static_cast<void>(_send(TerminalFrameKind::Input, { 0x1b }, _descriptor.Generation));
        }
        for (const auto& line : batch.Messages)
        {
            try
            {
                const auto input = til::u16u8(line);
                static_cast<void>(_send(
                    TerminalFrameKind::Input,
                    { input.begin(), input.end() },
                    _descriptor.Generation));
            }
            catch (...)
            {
            }
        }
    }

    void TaskRuntimeConnection::Resize(const uint32_t rows, const uint32_t columns)
    {
        const auto payload = EncodeResizePayload(columns, rows);
        static_cast<void>(_send(
            TerminalFrameKind::Resize,
            { payload.begin(), payload.end() },
            _descriptor.Generation));
    }

    void TaskRuntimeConnection::Close() noexcept
    {
        if (_stopEvent)
        {
            SetEvent(_stopEvent.get());
        }
        {
            const std::scoped_lock lock{ _writeMutex };
            _channel.CancelPendingIo();
        }
        if (_reader.joinable() && _reader.get_id() != std::this_thread::get_id())
        {
            _reader.join();
        }
        {
            const std::scoped_lock lock{ _writeMutex };
            _channel.Close();
        }
        if (State() < ConnectionState::Closed)
        {
            _transition(ConnectionState::Closed);
        }
    }

    winrt::guid TaskRuntimeConnection::SessionId() const noexcept
    {
        try
        {
            return winrt::guid{ winrt::to_hstring(_descriptor.TerminalSessionId) };
        }
        catch (...)
        {
            return {};
        }
    }

    ConnectionState TaskRuntimeConnection::State() const noexcept
    {
        const std::scoped_lock lock{ _stateMutex };
        return _state;
    }

    void TaskRuntimeConnection::_transition(const ConnectionState state) noexcept
    try
    {
        {
            const std::scoped_lock lock{ _stateMutex };
            if (state < _state || state == _state)
            {
                return;
            }
            _state = state;
        }
        StateChanged.raise(*this, nullptr);
    }
    catch (...)
    {
    }

    void TaskRuntimeConnection::_raiseOutput(const std::wstring_view output) noexcept
    try
    {
        if (!output.empty())
        {
            TerminalOutput.raise(winrt_wstring_to_array_view(output));
        }
    }
    catch (...)
    {
    }
}
