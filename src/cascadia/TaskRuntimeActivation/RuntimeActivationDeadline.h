// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeActivationTypes.h"

#include <optional>

namespace Microsoft::Terminal::TaskRuntime
{
    [[nodiscard]] std::optional<RuntimeActivationDeadline> MakeRuntimeActivationDeadline(
        RuntimeActivationMonotonicMilliseconds now,
        uint32_t timeoutMilliseconds) noexcept;

    [[nodiscard]] bool RuntimeActivationDeadlineReached(
        RuntimeActivationDeadline deadline,
        RuntimeActivationMonotonicMilliseconds now) noexcept;

    [[nodiscard]] RuntimeActivationMonotonicMilliseconds RuntimeActivationRemainingMilliseconds(
        RuntimeActivationDeadline deadline,
        RuntimeActivationMonotonicMilliseconds now) noexcept;
}
