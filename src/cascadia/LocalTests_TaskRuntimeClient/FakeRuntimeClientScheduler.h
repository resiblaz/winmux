// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientScheduler.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace TaskRuntimeClientLocalTests
{
    using namespace Microsoft::Terminal::TaskRuntime;

    class FakeRuntimeClientScheduler final : public IRuntimeClientScheduler
    {
    public:
        explicit FakeRuntimeClientScheduler(
            const RuntimeClientMonotonicMilliseconds initialNow = 0) noexcept :
            _now{ initialNow },
            _stateChanged{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
            _deadlineBlocked{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
            _releaseDeadline{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }
        {
        }

        ~FakeRuntimeClientScheduler() override
        {
            for (const auto handle : std::array{ _stateChanged, _deadlineBlocked, _releaseDeadline })
            {
                if (handle)
                {
                    CloseHandle(handle);
                }
            }
        }

        FakeRuntimeClientScheduler(const FakeRuntimeClientScheduler&) = delete;
        FakeRuntimeClientScheduler& operator=(const FakeRuntimeClientScheduler&) = delete;

        [[nodiscard]] bool Valid() const noexcept
        {
            return _stateChanged && _deadlineBlocked && _releaseDeadline;
        }

        [[nodiscard]] RuntimeClientMonotonicMilliseconds Now() noexcept override
        {
            std::scoped_lock lock{ _mutex };
            return _now;
        }

        [[nodiscard]] RuntimeClientScheduledWaitResult WaitUntil(
            const RuntimeClientMonotonicMilliseconds deadline,
            const std::span<const HANDLE> wakeEvents) noexcept override
        {
            if (!Valid() || wakeEvents.size() > MaximumRuntimeClientSchedulerWakeHandles ||
                std::ranges::any_of(wakeEvents, [](const HANDLE handle) { return handle == nullptr; }))
            {
                return _failed(ERROR_INVALID_PARAMETER);
            }

            try
            {
                std::scoped_lock lock{ _mutex };
                ++_waitCalls;
                _observedDeadlines.emplace_back(deadline);
            }
            catch (...)
            {
                _markWaitEntered();
                return _complete(_failed(ERROR_NOT_ENOUGH_MEMORY));
            }
            if (const auto wake = _pollWake(wakeEvents))
            {
                _markWaitEntered();
                return _complete(*wake);
            }

            std::optional<DWORD> injectedFailure;
            try
            {
                std::scoped_lock lock{ _mutex };
                injectedFailure = std::exchange(_nextFailure, std::nullopt);
            }
            catch (...)
            {
                _markWaitEntered();
                return _complete(_failed(ERROR_NOT_ENOUGH_MEMORY));
            }
            if (injectedFailure)
            {
                _markWaitEntered();
                return _complete(_failed(*injectedFailure));
            }

            std::shared_ptr<WaitRegistration> registration;
            try
            {
                registration = std::make_shared<WaitRegistration>(deadline);
            }
            catch (...)
            {
                _markWaitEntered();
                return _complete(_failed(ERROR_NOT_ENOUGH_MEMORY));
            }
            if (!registration->Changed)
            {
                _markWaitEntered();
                return _complete(_failed(GetLastError()));
            }

            bool registered{};
            try
            {
                std::scoped_lock lock{ _mutex };
                registration->Id = _nextWaitId++;
                _waiters.emplace_back(registration);
                ++_enteredWaits;
                registered = true;
                SetEvent(_stateChanged);
            }
            catch (...)
            {
                _markWaitEntered();
                return _complete(_failed(ERROR_NOT_ENOUGH_MEMORY));
            }

            const auto finish = [&](RuntimeClientScheduledWaitResult result) noexcept {
                if (registered)
                {
                    _remove(registration->Id);
                }
                return _complete(result);
            };

            for (;;)
            {
                if (const auto wake = _pollWake(wakeEvents))
                {
                    return finish(*wake);
                }

                bool due{};
                {
                    std::scoped_lock lock{ _mutex };
                    due = _now >= deadline;
                    if (!due)
                    {
                        ResetEvent(registration->Changed);
                    }
                }
                if (due)
                {
                    bool blockDeadline{};
                    {
                        std::scoped_lock lock{ _mutex };
                        blockDeadline = std::exchange(_blockNextDeadline, false);
                    }
                    if (blockDeadline)
                    {
                        SetEvent(_deadlineBlocked);
                        const auto released = WaitForSingleObject(_releaseDeadline, INFINITE);
                        if (released != WAIT_OBJECT_0)
                        {
                            return finish(_failed(
                                released == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE));
                        }
                    }
                    // Match production semantics: a lifecycle wake racing the
                    // exact deadline wins over DeadlineReached.
                    if (const auto wake = _pollWake(wakeEvents))
                    {
                        return finish(*wake);
                    }
                    return finish({
                        RuntimeClientScheduledWaitStatus::DeadlineReached,
                        InvalidRuntimeClientSchedulerWakeIndex,
                        ERROR_SUCCESS,
                    });
                }

                std::array<HANDLE, MaximumRuntimeClientSchedulerWakeHandles + 1> handles{};
                std::copy(wakeEvents.begin(), wakeEvents.end(), handles.begin());
                handles[wakeEvents.size()] = registration->Changed;
                const auto waited = WaitForMultipleObjects(
                    static_cast<DWORD>(wakeEvents.size() + 1),
                    handles.data(),
                    FALSE,
                    INFINITE);
                if (waited >= WAIT_OBJECT_0 &&
                    waited < WAIT_OBJECT_0 + static_cast<DWORD>(wakeEvents.size()))
                {
                    return finish({
                        RuntimeClientScheduledWaitStatus::WakeSignaled,
                        static_cast<size_t>(waited - WAIT_OBJECT_0),
                        ERROR_SUCCESS,
                    });
                }
                if (waited == WAIT_OBJECT_0 + static_cast<DWORD>(wakeEvents.size()))
                {
                    continue;
                }
                return finish(_failed(waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE));
            }
        }

        [[nodiscard]] bool AdvanceTo(
            const RuntimeClientMonotonicMilliseconds now) noexcept
        {
            std::scoped_lock lock{ _mutex };
            if (now < _now)
            {
                return false;
            }
            _now = now;
            _signalWaitersLocked();
            return true;
        }

        [[nodiscard]] bool AdvanceBy(
            const RuntimeClientMonotonicMilliseconds delta) noexcept
        {
            std::scoped_lock lock{ _mutex };
            const auto maximum = (std::numeric_limits<RuntimeClientMonotonicMilliseconds>::max)();
            _now = delta > maximum - _now ? maximum : _now + delta;
            _signalWaitersLocked();
            return true;
        }

        void Pulse() noexcept
        {
            std::scoped_lock lock{ _mutex };
            _signalWaitersLocked();
        }

        [[nodiscard]] bool BlockNextDeadlineReturn() noexcept
        {
            std::scoped_lock lock{ _mutex };
            if (_blockNextDeadline)
            {
                return false;
            }
            ResetEvent(_deadlineBlocked);
            ResetEvent(_releaseDeadline);
            _blockNextDeadline = true;
            return true;
        }

        [[nodiscard]] DWORD WaitForDeadlineBlocked(const DWORD timeoutMilliseconds = 2000) const noexcept
        {
            return WaitForSingleObject(_deadlineBlocked, timeoutMilliseconds);
        }

        void ReleaseDeadline() const noexcept
        {
            SetEvent(_releaseDeadline);
        }

        [[nodiscard]] bool FailNextWait(const DWORD systemError = ERROR_GEN_FAILURE) noexcept
        {
            std::scoped_lock lock{ _mutex };
            if (_nextFailure)
            {
                return false;
            }
            _nextFailure = systemError == ERROR_SUCCESS ? ERROR_GEN_FAILURE : systemError;
            return true;
        }

        [[nodiscard]] bool WaitForWaitEntered(
            const size_t expectedWaitCalls,
            const DWORD timeoutMilliseconds = 2000) const noexcept
        {
            const auto end = GetTickCount64() + timeoutMilliseconds;
            for (;;)
            {
                {
                    std::scoped_lock lock{ _mutex };
                    if (_enteredWaits >= expectedWaitCalls)
                    {
                        return true;
                    }
                    ResetEvent(_stateChanged);
                }
                const auto now = GetTickCount64();
                if (now >= end)
                {
                    return false;
                }
                if (WaitForSingleObject(
                        _stateChanged,
                        static_cast<DWORD>(end - now)) != WAIT_OBJECT_0)
                {
                    return false;
                }
            }
        }

        [[nodiscard]] std::vector<RuntimeClientMonotonicMilliseconds> ObservedDeadlines() const
        {
            std::scoped_lock lock{ _mutex };
            return _observedDeadlines;
        }

        [[nodiscard]] bool WaitForCompletedWaits(
            const size_t expectedCompletedWaits,
            const DWORD timeoutMilliseconds = 2000) const noexcept
        {
            const auto end = GetTickCount64() + timeoutMilliseconds;
            for (;;)
            {
                {
                    std::scoped_lock lock{ _mutex };
                    if (_deadlineWakes + _signalWakes + _failedWaits >= expectedCompletedWaits)
                    {
                        return true;
                    }
                    ResetEvent(_stateChanged);
                }
                const auto now = GetTickCount64();
                if (now >= end ||
                    WaitForSingleObject(_stateChanged, static_cast<DWORD>(end - now)) !=
                        WAIT_OBJECT_0)
                {
                    return false;
                }
            }
        }

        [[nodiscard]] size_t WaitCallCount() const noexcept
        {
            std::scoped_lock lock{ _mutex };
            return _waitCalls;
        }

        [[nodiscard]] size_t PendingWaitCount() const noexcept
        {
            std::scoped_lock lock{ _mutex };
            return _waiters.size();
        }

        [[nodiscard]] size_t DeadlineWakeCount() const noexcept
        {
            std::scoped_lock lock{ _mutex };
            return _deadlineWakes;
        }

        [[nodiscard]] size_t SignalWakeCount() const noexcept
        {
            std::scoped_lock lock{ _mutex };
            return _signalWakes;
        }

        [[nodiscard]] size_t FailedWaitCount() const noexcept
        {
            std::scoped_lock lock{ _mutex };
            return _failedWaits;
        }

    private:
        struct WaitRegistration
        {
            explicit WaitRegistration(const RuntimeClientMonotonicMilliseconds deadline) noexcept :
                Deadline{ deadline },
                Changed{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }
            {
            }

            ~WaitRegistration()
            {
                if (Changed)
                {
                    CloseHandle(Changed);
                }
            }

            uint64_t Id{};
            RuntimeClientMonotonicMilliseconds Deadline{};
            HANDLE Changed{};
        };

        [[nodiscard]] static RuntimeClientScheduledWaitResult _failed(const DWORD error) noexcept
        {
            return {
                RuntimeClientScheduledWaitStatus::Failed,
                InvalidRuntimeClientSchedulerWakeIndex,
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error,
            };
        }

        [[nodiscard]] static std::optional<RuntimeClientScheduledWaitResult> _pollWake(
            const std::span<const HANDLE> wakeEvents) noexcept
        {
            if (wakeEvents.empty())
            {
                return std::nullopt;
            }
            const auto waited = WaitForMultipleObjects(
                static_cast<DWORD>(wakeEvents.size()),
                wakeEvents.data(),
                FALSE,
                0);
            if (waited >= WAIT_OBJECT_0 &&
                waited < WAIT_OBJECT_0 + static_cast<DWORD>(wakeEvents.size()))
            {
                return RuntimeClientScheduledWaitResult{
                    RuntimeClientScheduledWaitStatus::WakeSignaled,
                    static_cast<size_t>(waited - WAIT_OBJECT_0),
                    ERROR_SUCCESS,
                };
            }
            if (waited == WAIT_TIMEOUT)
            {
                return std::nullopt;
            }
            return _failed(waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE);
        }

        RuntimeClientScheduledWaitResult _complete(
            RuntimeClientScheduledWaitResult result) noexcept
        {
            std::scoped_lock lock{ _mutex };
            switch (result.Status)
            {
            case RuntimeClientScheduledWaitStatus::DeadlineReached:
                ++_deadlineWakes;
                break;
            case RuntimeClientScheduledWaitStatus::WakeSignaled:
                ++_signalWakes;
                break;
            case RuntimeClientScheduledWaitStatus::Failed:
                ++_failedWaits;
                break;
            }
            SetEvent(_stateChanged);
            return result;
        }

        void _markWaitEntered() noexcept
        {
            std::scoped_lock lock{ _mutex };
            ++_enteredWaits;
            SetEvent(_stateChanged);
        }

        void _remove(const uint64_t id) noexcept
        {
            std::scoped_lock lock{ _mutex };
            const auto found = std::ranges::find_if(_waiters, [id](const auto& waiter) {
                return waiter && waiter->Id == id;
            });
            if (found != _waiters.end())
            {
                _waiters.erase(found);
            }
            SetEvent(_stateChanged);
        }

        void _signalWaitersLocked() noexcept
        {
            for (const auto& waiter : _waiters)
            {
                if (waiter)
                {
                    SetEvent(waiter->Changed);
                }
            }
            SetEvent(_stateChanged);
        }

        mutable std::mutex _mutex;
        RuntimeClientMonotonicMilliseconds _now{};
        HANDLE _stateChanged{};
        HANDLE _deadlineBlocked{};
        HANDLE _releaseDeadline{};
        std::vector<std::shared_ptr<WaitRegistration>> _waiters;
        std::vector<RuntimeClientMonotonicMilliseconds> _observedDeadlines;
        std::optional<DWORD> _nextFailure;
        uint64_t _nextWaitId{ 1 };
        size_t _waitCalls{};
        size_t _enteredWaits{};
        size_t _deadlineWakes{};
        size_t _signalWakes{};
        size_t _failedWaits{};
        bool _blockNextDeadline{};
    };
}
