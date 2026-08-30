// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeActivationServices.h"

#include "RuntimeActivationDeadline.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        constexpr DWORD MaximumFiniteWaitMilliseconds{ INFINITE - 1 };

        RuntimeActivationWaitResult _failed(const DWORD error) noexcept
        {
            return {
                RuntimeActivationWaitStatus::Failed,
                InvalidRuntimeActivationWaitIndex,
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error,
            };
        }

        std::optional<RuntimeActivationWaitResult> _poll(
            const std::span<const HANDLE> wakeHandles) noexcept
        {
            if (wakeHandles.empty())
            {
                return std::nullopt;
            }
            const auto waited = WaitForMultipleObjects(
                static_cast<DWORD>(wakeHandles.size()),
                wakeHandles.data(),
                FALSE,
                0);
            if (waited >= WAIT_OBJECT_0 &&
                waited < WAIT_OBJECT_0 + static_cast<DWORD>(wakeHandles.size()))
            {
                return RuntimeActivationWaitResult{
                    RuntimeActivationWaitStatus::WakeSignaled,
                    static_cast<size_t>(waited - WAIT_OBJECT_0),
                    ERROR_SUCCESS,
                };
            }
            if (waited >= WAIT_ABANDONED_0 &&
                waited < WAIT_ABANDONED_0 + static_cast<DWORD>(wakeHandles.size()))
            {
                return RuntimeActivationWaitResult{
                    RuntimeActivationWaitStatus::WakeAbandoned,
                    static_cast<size_t>(waited - WAIT_ABANDONED_0),
                    ERROR_SUCCESS,
                };
            }
            if (waited == WAIT_TIMEOUT)
            {
                return std::nullopt;
            }
            return _failed(waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE);
        }

        class RuntimeActivationWindowsServices final : public IRuntimeActivationServices
        {
        public:
            RuntimeActivationMonotonicMilliseconds Now() noexcept override
            {
                return GetTickCount64();
            }

            RuntimeActivationWaitResult WaitUntil(
                const RuntimeActivationDeadline deadline,
                const std::span<const HANDLE> wakeHandles) noexcept override
            {
                if (!deadline || wakeHandles.size() > MaximumRuntimeActivationWaitHandles ||
                    std::ranges::any_of(wakeHandles, [](const HANDLE handle) {
                        return handle == nullptr || handle == INVALID_HANDLE_VALUE;
                    }))
                {
                    return _failed(ERROR_INVALID_PARAMETER);
                }

                for (;;)
                {
                    // The lowest indexed ready handle wins, including at the
                    // exact deadline. Callers put cancellation first.
                    if (const auto wake = _poll(wakeHandles))
                    {
                        return *wake;
                    }

                    const auto now = Now();
                    const auto remaining = RuntimeActivationRemainingMilliseconds(deadline, now);
                    if (remaining == 0)
                    {
                        if (const auto wake = _poll(wakeHandles))
                        {
                            return *wake;
                        }
                        return {
                            RuntimeActivationWaitStatus::DeadlineReached,
                            InvalidRuntimeActivationWaitIndex,
                            ERROR_SUCCESS,
                        };
                    }

                    const auto waitMilliseconds = static_cast<DWORD>((std::min)(
                        remaining,
                        static_cast<RuntimeActivationMonotonicMilliseconds>(
                            MaximumFiniteWaitMilliseconds)));
                    if (wakeHandles.empty())
                    {
                        Sleep(waitMilliseconds);
                        continue;
                    }

                    const auto waited = WaitForMultipleObjects(
                        static_cast<DWORD>(wakeHandles.size()),
                        wakeHandles.data(),
                        FALSE,
                        waitMilliseconds);
                    if (waited >= WAIT_OBJECT_0 &&
                        waited < WAIT_OBJECT_0 + static_cast<DWORD>(wakeHandles.size()))
                    {
                        return {
                            RuntimeActivationWaitStatus::WakeSignaled,
                            static_cast<size_t>(waited - WAIT_OBJECT_0),
                            ERROR_SUCCESS,
                        };
                    }
                    if (waited >= WAIT_ABANDONED_0 &&
                        waited < WAIT_ABANDONED_0 + static_cast<DWORD>(wakeHandles.size()))
                    {
                        return {
                            RuntimeActivationWaitStatus::WakeAbandoned,
                            static_cast<size_t>(waited - WAIT_ABANDONED_0),
                            ERROR_SUCCESS,
                        };
                    }
                    if (waited != WAIT_TIMEOUT)
                    {
                        return _failed(
                            waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE);
                    }
                }
            }
        };
    }

    std::shared_ptr<IRuntimeActivationServices> MakeRuntimeActivationWindowsServices()
    {
        try
        {
            return std::make_shared<RuntimeActivationWindowsServices>();
        }
        catch (...)
        {
            return {};
        }
    }
}
