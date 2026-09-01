// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "StateTransition.h"
#include "TaskEnums.h"

namespace Microsoft::Terminal::TaskCore
{
    [[nodiscard]] bool IsTaskGitCheckpointTerminal(TaskGitCheckpointState state) noexcept;
    [[nodiscard]] StateTransitionDecision<TaskGitCheckpointState>
    EvaluateTaskGitCheckpointTransition(
        TaskGitCheckpointState current,
        TaskGitCheckpointState requested) noexcept;
}
