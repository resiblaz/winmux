// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeActivationDeadline.h"

namespace Microsoft::Terminal::TaskRuntime
{
    std::optional<RuntimeActivationDeadline> MakeRuntimeActivationDeadline(
        const RuntimeActivationMonotonicMilliseconds now,
        const uint32_t timeoutMilliseconds) noexcept
    {
        if (timeoutMilliseconds == 0 ||
            now == (std::numeric_limits<RuntimeActivationMonotonicMilliseconds>::max)())
        {
            return std::nullopt;
        }

        const auto maximum =
            (std::numeric_limits<RuntimeActivationMonotonicMilliseconds>::max)();
        const auto value = timeoutMilliseconds > maximum - now ?
                               maximum :
                               now + timeoutMilliseconds;
        return value > now ?
                   std::optional<RuntimeActivationDeadline>{ RuntimeActivationDeadline{ value } } :
                   std::nullopt;
    }

    bool RuntimeActivationDeadlineReached(
        const RuntimeActivationDeadline deadline,
        const RuntimeActivationMonotonicMilliseconds now) noexcept
    {
        return !deadline || now >= deadline.Value;
    }

    RuntimeActivationMonotonicMilliseconds RuntimeActivationRemainingMilliseconds(
        const RuntimeActivationDeadline deadline,
        const RuntimeActivationMonotonicMilliseconds now) noexcept
    {
        return RuntimeActivationDeadlineReached(deadline, now) ?
                   0 :
                   deadline.Value - now;
    }
}
