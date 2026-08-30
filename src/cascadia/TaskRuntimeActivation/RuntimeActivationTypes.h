// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    using RuntimeActivationMonotonicMilliseconds = uint64_t;

    inline constexpr size_t MaximumRuntimeActivationWaitHandles{ 8 };
    inline constexpr size_t InvalidRuntimeActivationWaitIndex{
        (std::numeric_limits<size_t>::max)()
    };

    struct RuntimeActivationDeadline
    {
        RuntimeActivationMonotonicMilliseconds Value{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Value != 0;
        }
    };

    enum class RuntimeActivationWaitStatus
    {
        DeadlineReached,
        WakeSignaled,
        WakeAbandoned,
        Failed,
    };

    struct RuntimeActivationWaitResult
    {
        RuntimeActivationWaitStatus Status{ RuntimeActivationWaitStatus::Failed };
        size_t WakeIndex{ InvalidRuntimeActivationWaitIndex };
        DWORD SystemError{};
    };
}
