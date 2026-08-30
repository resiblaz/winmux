// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "StateTransition.h"
#include "TaskEnums.h"

namespace Microsoft::Terminal::TaskCore
{
    [[nodiscard]] bool IsTaskRunTerminal(TaskRunState state) noexcept;
    [[nodiscard]] bool IsTaskRunActive(TaskRunState state) noexcept;
    [[nodiscard]] StateTransitionDecision<TaskRunState> EvaluateTaskRunTransition(
        TaskRunState current,
        TaskRunState requested) noexcept;
}
