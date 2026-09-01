// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "StateTransition.h"
#include "TaskEnums.h"

namespace Microsoft::Terminal::TaskCore
{
    [[nodiscard]] bool IsTaskGitWorktreeSetupState(
        TaskGitWorktreeState state) noexcept;
    [[nodiscard]] StateTransitionDecision<TaskGitWorktreeState>
    EvaluateTaskGitWorktreeSetupTransition(
        TaskGitWorktreeState current,
        TaskGitWorktreeState requested) noexcept;
    [[nodiscard]] bool IsTaskGitWorktreeFinalizationState(
        TaskGitWorktreeState state) noexcept;
    [[nodiscard]] StateTransitionDecision<TaskGitWorktreeState>
    EvaluateTaskGitWorktreeFinalizationTransition(
        TaskGitWorktreeState current,
        TaskGitWorktreeState requested) noexcept;
}
