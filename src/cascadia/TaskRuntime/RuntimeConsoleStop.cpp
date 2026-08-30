// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeConsoleStop.h"

#include <atomic>

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        std::atomic<HANDLE> _stopEvent{};
        std::atomic<HANDLE> _cleanupCompleteEvent{};
        std::atomic<HANDLE> _handlersIdleEvent{};
        std::atomic_uint32_t _activeHandlers{};
        std::atomic_bool _closing{ true };

        void _endHandler(const HANDLE handlersIdle) noexcept
        {
            if (_activeHandlers.fetch_sub(1, std::memory_order_acq_rel) == 1 && handlersIdle)
            {
                SetEvent(handlersIdle);
            }
        }

        bool _beginHandler(HANDLE& handlersIdle) noexcept
        {
            if (_closing.load(std::memory_order_acquire))
            {
                return false;
            }
            _activeHandlers.fetch_add(1, std::memory_order_acq_rel);
            // Once counted, teardown retains the event until this handler
            // releases its rundown reference.
            handlersIdle = _handlersIdleEvent.load(std::memory_order_acquire);
            if (_closing.load(std::memory_order_acquire))
            {
                _endHandler(handlersIdle);
                return false;
            }
            return true;
        }

        BOOL WINAPI _consoleHandler(const DWORD event) noexcept
        {
            HANDLE handlersIdle{};
            if (!_beginHandler(handlersIdle))
            {
                return FALSE;
            }

            BOOL handled{};
            switch (event)
            {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
                if (const auto handle = _stopEvent.load(std::memory_order_acquire))
                {
                    SetEvent(handle);
                    handled = TRUE;
                }
                break;
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                if (const auto handle = _stopEvent.load(std::memory_order_acquire))
                {
                    SetEvent(handle);
                    handled = TRUE;
                    if (const auto complete = _cleanupCompleteEvent.load(std::memory_order_acquire))
                    {
                        // The operating system owns the outer shutdown
                        // deadline. Keep this handler alive until the main
                        // thread has joined the server and every session.
                        (void)WaitForSingleObject(complete, INFINITE);
                    }
                }
                break;
            }
            _endHandler(handlersIdle);
            return handled;
        }
    }

    RuntimeConsoleStop::RuntimeConsoleStop() noexcept :
        _event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
        _cleanupComplete{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
        _handlersIdle{ CreateEventW(nullptr, TRUE, TRUE, nullptr) }
    {
        HANDLE expected{};
        if (_event && _cleanupComplete && _handlersIdle &&
            _stopEvent.compare_exchange_strong(expected, _event, std::memory_order_acq_rel))
        {
            _cleanupCompleteEvent.store(_cleanupComplete, std::memory_order_release);
            _handlersIdleEvent.store(_handlersIdle, std::memory_order_release);
            _activeHandlers.store(0, std::memory_order_release);
            _closing.store(false, std::memory_order_release);
            if (SetConsoleCtrlHandler(_consoleHandler, TRUE))
            {
                _installed = true;
                return;
            }
            _closing.store(true, std::memory_order_release);
            _handlersIdleEvent.store(nullptr, std::memory_order_release);
            _cleanupCompleteEvent.store(nullptr, std::memory_order_release);
            _stopEvent.store(nullptr, std::memory_order_release);
        }
        if (_event)
        {
            CloseHandle(_event);
            _event = nullptr;
        }
        if (_cleanupComplete)
        {
            CloseHandle(_cleanupComplete);
            _cleanupComplete = nullptr;
        }
        if (_handlersIdle)
        {
            CloseHandle(_handlersIdle);
            _handlersIdle = nullptr;
        }
    }

    RuntimeConsoleStop::~RuntimeConsoleStop()
    {
        if (_installed)
        {
            Complete();
            SetConsoleCtrlHandler(_consoleHandler, FALSE);
            _closing.store(true, std::memory_order_release);
            ResetEvent(_handlersIdle);
            if (_activeHandlers.load(std::memory_order_acquire) != 0)
            {
                (void)WaitForSingleObject(_handlersIdle, INFINITE);
            }
            _handlersIdleEvent.store(nullptr, std::memory_order_release);
            _cleanupCompleteEvent.store(nullptr, std::memory_order_release);
            _stopEvent.store(nullptr, std::memory_order_release);
        }
        if (_event)
        {
            CloseHandle(_event);
        }
        if (_cleanupComplete)
        {
            CloseHandle(_cleanupComplete);
        }
        if (_handlersIdle)
        {
            CloseHandle(_handlersIdle);
        }
    }

    RuntimeConsoleStop::operator bool() const noexcept
    {
        return _event != nullptr && _installed;
    }

    HANDLE RuntimeConsoleStop::Handle() const noexcept
    {
        return _event;
    }

    void RuntimeConsoleStop::Complete() noexcept
    {
        if (_cleanupComplete)
        {
            SetEvent(_cleanupComplete);
        }
    }
}
