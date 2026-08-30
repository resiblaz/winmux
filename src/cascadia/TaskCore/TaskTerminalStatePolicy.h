// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "StateTransition.h"
#include "TaskEnums.h"

#include <cstdint>

namespace Microsoft::Terminal::TaskCore
{
    struct TaskTerminalTransitionContext
    {
        uint64_t CurrentGeneration{};
        uint64_t RequestedGeneration{};
        bool ExplicitOpen{};
        bool HasActiveRun{};
    };

    [[nodiscard]] StateTransitionDecision<TaskTerminalState> EvaluateTaskTerminalTransition(
        TaskTerminalState current,
        TaskTerminalState requested,
        const TaskTerminalTransitionContext& context) noexcept;
}
