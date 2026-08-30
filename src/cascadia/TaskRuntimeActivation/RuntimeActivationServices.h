// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeActivationTypes.h"

#include <memory>
#include <span>

namespace Microsoft::Terminal::TaskRuntime
{
    // Synchronous monotonic-time seam. Implementations own no callbacks or
    // worker threads. Wake handles are priority ordered by their span index.
    class IRuntimeActivationServices
    {
    public:
        virtual ~IRuntimeActivationServices() = default;

        [[nodiscard]] virtual RuntimeActivationMonotonicMilliseconds Now() noexcept = 0;
        [[nodiscard]] virtual RuntimeActivationWaitResult WaitUntil(
            RuntimeActivationDeadline deadline,
            std::span<const HANDLE> wakeHandles) noexcept = 0;
    };

    [[nodiscard]] std::shared_ptr<IRuntimeActivationServices>
        MakeRuntimeActivationWindowsServices();
}
