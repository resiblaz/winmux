// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskGitCheckpointStatePolicy.h"

namespace Microsoft::Terminal::TaskCore
{
    bool IsTaskGitCheckpointTerminal(const TaskGitCheckpointState state) noexcept
    {
        return state == TaskGitCheckpointState::Succeeded ||
               state == TaskGitCheckpointState::NotNeeded ||
               state == TaskGitCheckpointState::Failed;
    }

    StateTransitionDecision<TaskGitCheckpointState>
    EvaluateTaskGitCheckpointTransition(
        const TaskGitCheckpointState current,
        const TaskGitCheckpointState requested) noexcept
    {
        if (current == requested ||
            (IsTaskGitCheckpointTerminal(current) &&
             current != TaskGitCheckpointState::Failed) ||
            requested == TaskGitCheckpointState::Pending)
        {
            return StateTransitionDecision<TaskGitCheckpointState>::Reject(
                current,
                TransitionError::InvalidTransition);
        }

        const auto allowed =
            (current == TaskGitCheckpointState::Pending &&
             requested == TaskGitCheckpointState::Inspecting) ||
            (current == TaskGitCheckpointState::Inspecting &&
             (requested == TaskGitCheckpointState::Committing ||
              requested == TaskGitCheckpointState::NotNeeded ||
              requested == TaskGitCheckpointState::Failed)) ||
            (current == TaskGitCheckpointState::Committing &&
             (requested == TaskGitCheckpointState::Succeeded ||
              requested == TaskGitCheckpointState::Failed)) ||
            (current == TaskGitCheckpointState::Failed &&
             (requested == TaskGitCheckpointState::Inspecting ||
              requested == TaskGitCheckpointState::Committing));
        return allowed ?
                   StateTransitionDecision<TaskGitCheckpointState>::Permit(requested) :
                   StateTransitionDecision<TaskGitCheckpointState>::Reject(
                       current,
                       TransitionError::InvalidTransition);
    }
}
