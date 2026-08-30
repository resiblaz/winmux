// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace Microsoft::Terminal::TaskRuntime
{
    using RuntimeClientMonotonicMilliseconds = uint64_t;

    inline constexpr size_t MaximumRuntimeClientSchedulerWakeHandles{ 8 };
    inline constexpr size_t InvalidRuntimeClientSchedulerWakeIndex{
        (std::numeric_limits<size_t>::max)()
    };

    enum class RuntimeClientScheduledWaitStatus
    {
        DeadlineReached,
        WakeSignaled,
        Failed,
    };

    struct RuntimeClientScheduledWaitResult
    {
        RuntimeClientScheduledWaitStatus Status{ RuntimeClientScheduledWaitStatus::Failed };
        size_t WakeIndex{ InvalidRuntimeClientSchedulerWakeIndex };
        DWORD SystemError{};
    };

    // Synchronous monotonic-time seam. Implementations own no callbacks or
    // background threads; the calling supervisor remains the sole state owner.
    class IRuntimeClientScheduler
    {
    public:
        virtual ~IRuntimeClientScheduler() = default;

        [[nodiscard]] virtual RuntimeClientMonotonicMilliseconds Now() noexcept = 0;
        [[nodiscard]] virtual RuntimeClientScheduledWaitResult WaitUntil(
            RuntimeClientMonotonicMilliseconds deadline,
            std::span<const HANDLE> wakeEvents) noexcept = 0;
    };

    [[nodiscard]] RuntimeClientMonotonicMilliseconds RuntimeClientDeadlineAfter(
        RuntimeClientMonotonicMilliseconds now,
        uint32_t delayMilliseconds) noexcept;

    [[nodiscard]] std::shared_ptr<IRuntimeClientScheduler> MakeRuntimeClientScheduler();
}
