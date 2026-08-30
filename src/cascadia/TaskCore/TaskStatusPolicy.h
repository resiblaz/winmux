// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "StateTransition.h"
#include "TaskEnums.h"

namespace Microsoft::Terminal::TaskCore
{
    enum class TaskStatusAuthority
    {
        User,
        Agent,
        SystemRecovery,
    };

    struct TaskStatusTransitionContext
    {
        bool HasActiveRun{};
        bool HasUnclaimedMessages{};
        bool HasPendingAutomaticRetry{};
    };

    [[nodiscard]] StateTransitionDecision<TaskStatus> EvaluateTaskStatusTransition(
        TaskStatus current,
        TaskStatus requested,
        TaskStatusAuthority authority,
        const TaskStatusTransitionContext& context) noexcept;
}
