// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientScheduler.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        constexpr DWORD MaximumFiniteWaitMilliseconds{ INFINITE - 1 };

        RuntimeClientScheduledWaitResult _failed(const DWORD error) noexcept
        {
            return {
                RuntimeClientScheduledWaitStatus::Failed,
                InvalidRuntimeClientSchedulerWakeIndex,
                error,
            };
        }

        std::optional<RuntimeClientScheduledWaitResult> _pollWake(
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

        class Win32RuntimeClientScheduler final : public IRuntimeClientScheduler
        {
        public:
            [[nodiscard]] RuntimeClientMonotonicMilliseconds Now() noexcept override
            {
                return GetTickCount64();
            }

            [[nodiscard]] RuntimeClientScheduledWaitResult WaitUntil(
                const RuntimeClientMonotonicMilliseconds deadline,
                const std::span<const HANDLE> wakeEvents) noexcept override
            {
                if (wakeEvents.size() > MaximumRuntimeClientSchedulerWakeHandles ||
                    std::ranges::any_of(wakeEvents, [](const HANDLE handle) { return handle == nullptr; }))
                {
                    return _failed(ERROR_INVALID_PARAMETER);
                }

                for (;;)
                {
                    if (const auto wake = _pollWake(wakeEvents))
                    {
                        return *wake;
                    }

                    const auto now = Now();
                    if (now >= deadline)
                    {
                        // A lifecycle wake that races the deadline wins.
                        if (const auto wake = _pollWake(wakeEvents))
                        {
                            return *wake;
                        }
                        return {
                            RuntimeClientScheduledWaitStatus::DeadlineReached,
                            InvalidRuntimeClientSchedulerWakeIndex,
                            ERROR_SUCCESS,
                        };
                    }

                    const auto remaining = deadline - now;
                    const auto waitMilliseconds = static_cast<DWORD>((std::min)(
                        remaining,
                        static_cast<RuntimeClientMonotonicMilliseconds>(MaximumFiniteWaitMilliseconds)));
                    if (wakeEvents.empty())
                    {
                        Sleep(waitMilliseconds);
                        continue;
                    }

                    const auto waited = WaitForMultipleObjects(
                        static_cast<DWORD>(wakeEvents.size()),
                        wakeEvents.data(),
                        FALSE,
                        waitMilliseconds);
                    if (waited >= WAIT_OBJECT_0 &&
                        waited < WAIT_OBJECT_0 + static_cast<DWORD>(wakeEvents.size()))
                    {
                        return {
                            RuntimeClientScheduledWaitStatus::WakeSignaled,
                            static_cast<size_t>(waited - WAIT_OBJECT_0),
                            ERROR_SUCCESS,
                        };
                    }
                    if (waited != WAIT_TIMEOUT)
                    {
                        return _failed(waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE);
                    }
                }
            }
        };
    }

    RuntimeClientMonotonicMilliseconds RuntimeClientDeadlineAfter(
        const RuntimeClientMonotonicMilliseconds now,
        const uint32_t delayMilliseconds) noexcept
    {
        const auto maximum = (std::numeric_limits<RuntimeClientMonotonicMilliseconds>::max)();
        return delayMilliseconds > maximum - now ? maximum : now + delayMilliseconds;
    }

    std::shared_ptr<IRuntimeClientScheduler> MakeRuntimeClientScheduler()
    {
        try
        {
            return std::make_shared<Win32RuntimeClientScheduler>();
        }
        catch (...)
        {
            return {};
        }
    }
}
