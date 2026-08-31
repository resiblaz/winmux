// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskGitWorktreeStatePolicy.h"

namespace Microsoft::Terminal::TaskCore
{
    bool IsTaskGitWorktreeSetupState(
        const TaskGitWorktreeState state) noexcept
    {
        return state == TaskGitWorktreeState::Planning ||
               state == TaskGitWorktreeState::CapturingSource ||
               state == TaskGitWorktreeState::Provisioning ||
               state == TaskGitWorktreeState::RestoringSource ||
               state == TaskGitWorktreeState::CreatingBaseline ||
               state == TaskGitWorktreeState::Ready ||
               state == TaskGitWorktreeState::Failed;
    }

    StateTransitionDecision<TaskGitWorktreeState>
    EvaluateTaskGitWorktreeSetupTransition(
        const TaskGitWorktreeState current,
        const TaskGitWorktreeState requested) noexcept
    {
        if (!IsTaskGitWorktreeSetupState(current) ||
            !IsTaskGitWorktreeSetupState(requested) || current == requested ||
            current == TaskGitWorktreeState::Ready)
        {
            return StateTransitionDecision<TaskGitWorktreeState>::Reject(
                current,
                TransitionError::InvalidTransition);
        }
        if (requested == TaskGitWorktreeState::Failed)
        {
            return StateTransitionDecision<TaskGitWorktreeState>::Permit(requested);
        }
        if (current == TaskGitWorktreeState::Failed)
        {
            const auto retry = requested == TaskGitWorktreeState::CapturingSource ||
                               requested == TaskGitWorktreeState::Provisioning ||
                               requested == TaskGitWorktreeState::RestoringSource ||
                               requested == TaskGitWorktreeState::CreatingBaseline;
            return retry ?
                       StateTransitionDecision<TaskGitWorktreeState>::Permit(
                           requested) :
                       StateTransitionDecision<TaskGitWorktreeState>::Reject(
                           current,
                           TransitionError::InvalidTransition);
        }

        const auto allowed =
            (current == TaskGitWorktreeState::Planning &&
             requested == TaskGitWorktreeState::CapturingSource) ||
            (current == TaskGitWorktreeState::CapturingSource &&
             requested == TaskGitWorktreeState::Provisioning) ||
            (current == TaskGitWorktreeState::Provisioning &&
             requested == TaskGitWorktreeState::RestoringSource) ||
            (current == TaskGitWorktreeState::RestoringSource &&
             requested == TaskGitWorktreeState::CreatingBaseline) ||
            (current == TaskGitWorktreeState::CreatingBaseline &&
             requested == TaskGitWorktreeState::Ready);
        return allowed ?
                   StateTransitionDecision<TaskGitWorktreeState>::Permit(requested) :
                   StateTransitionDecision<TaskGitWorktreeState>::Reject(
                       current,
                       TransitionError::InvalidTransition);
    }
}
