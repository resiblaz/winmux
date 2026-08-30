// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskRunStatePolicy.h"

namespace Microsoft::Terminal::TaskCore
{
    bool IsTaskRunTerminal(const TaskRunState state) noexcept
    {
        return state == TaskRunState::Completed ||
               state == TaskRunState::Failed ||
               state == TaskRunState::Cancelled ||
               state == TaskRunState::Interrupted;
    }

    bool IsTaskRunActive(const TaskRunState state) noexcept
    {
        return !IsTaskRunTerminal(state);
    }

    StateTransitionDecision<TaskRunState> EvaluateTaskRunTransition(
        const TaskRunState current,
        const TaskRunState requested) noexcept
    {
        if (current == requested)
        {
            return StateTransitionDecision<TaskRunState>::Permit(current, false);
        }
        if (IsTaskRunTerminal(current))
        {
            return StateTransitionDecision<TaskRunState>::Reject(current, TransitionError::InvalidTransition);
        }

        bool allowed{};
        switch (current)
        {
        case TaskRunState::Queued:
            allowed = requested == TaskRunState::Preparing || requested == TaskRunState::Cancelled || requested == TaskRunState::Interrupted;
            break;
        case TaskRunState::Preparing:
            allowed = requested == TaskRunState::WaitingForDirectory || requested == TaskRunState::Starting ||
                      requested == TaskRunState::Failed || requested == TaskRunState::Cancelled || requested == TaskRunState::Interrupted;
            break;
        case TaskRunState::WaitingForDirectory:
            allowed = requested == TaskRunState::Starting || requested == TaskRunState::Failed ||
                      requested == TaskRunState::Cancelled || requested == TaskRunState::Interrupted;
            break;
        case TaskRunState::Starting:
            allowed = requested == TaskRunState::Running || requested == TaskRunState::Failed ||
                      requested == TaskRunState::Cancelled || requested == TaskRunState::Interrupted;
            break;
        case TaskRunState::Running:
            allowed = IsTaskRunTerminal(requested);
            break;
        default:
            break;
        }

        return allowed ?
                   StateTransitionDecision<TaskRunState>::Permit(requested) :
                   StateTransitionDecision<TaskRunState>::Reject(current, TransitionError::InvalidTransition);
    }
}
