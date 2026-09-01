// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProjection.h"

#include "RuntimeTerminalChannel.h"

#include <winrt/Microsoft.Terminal.TerminalConnection.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <wil/resource.h>

namespace winrt::TerminalApp::implementation::tasks
{
    class TaskRuntimeConnection :
        public winrt::implements<
            TaskRuntimeConnection,
            winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection>
    {
    public:
        explicit TaskRuntimeConnection(
            TaskSurfaceDescriptor descriptor,
            std::function<void(std::wstring, uint64_t)> closeAuthorized);
        ~TaskRuntimeConnection();

        void Initialize(const Windows::Foundation::Collections::ValueSet&) noexcept {}
        void Start();
        void WriteInput(winrt::array_view<const char16_t> data);
        void Resize(uint32_t rows, uint32_t columns);
        void Close() noexcept;

        [[nodiscard]] winrt::guid SessionId() const noexcept;
        [[nodiscard]] Microsoft::Terminal::TerminalConnection::ConnectionState State() const noexcept;

        til::event<Microsoft::Terminal::TerminalConnection::TerminalOutputHandler> TerminalOutput;
        til::typed_event<Microsoft::Terminal::TerminalConnection::ITerminalConnection, Windows::Foundation::IInspectable> StateChanged;

    private:
        TaskSurfaceDescriptor _descriptor;
        std::function<void(std::wstring, uint64_t)> _closeAuthorized;
        mutable std::mutex _stateMutex;
        std::mutex _writeMutex;
        std::mutex _inputMutex;
        Microsoft::Terminal::TerminalConnection::ConnectionState _state{
            Microsoft::Terminal::TerminalConnection::ConnectionState::NotConnected
        };
        ::Microsoft::Terminal::TaskRuntime::RuntimeTerminalChannel _channel;
        wil::unique_handle _stopEvent;
        std::thread _reader;
        std::wstring _inputLine;
        uint64_t _nextClientSequence{ 2 };
        uint64_t _lastOutputSequence{};
        bool _lastInputWasCarriageReturn{};

        void _run() noexcept;
        void _transition(Microsoft::Terminal::TerminalConnection::ConnectionState state) noexcept;
        [[nodiscard]] bool _send(
            ::Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrameKind kind,
            std::vector<uint8_t> payload,
            uint64_t generation);
        void _raiseOutput(std::wstring_view output) noexcept;
        void _flushInputLine();
    };
}
