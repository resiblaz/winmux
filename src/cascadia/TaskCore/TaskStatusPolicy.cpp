// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStatusPolicy.h"

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        bool _isUserTransitionAllowed(const TaskStatus current, const TaskStatus requested) noexcept
        {
            switch (current)
            {
            case TaskStatus::Backlog:
                return requested == TaskStatus::Todo || requested == TaskStatus::Cancelled;
            case TaskStatus::Todo:
                return requested == TaskStatus::Backlog || requested == TaskStatus::InProgress || requested == TaskStatus::Cancelled;
            case TaskStatus::InProgress:
                return requested == TaskStatus::Todo || requested == TaskStatus::InReview || requested == TaskStatus::Blocked || requested == TaskStatus::Cancelled;
            case TaskStatus::InReview:
                return requested == TaskStatus::InProgress || requested == TaskStatus::Done || requested == TaskStatus::Cancelled;
            case TaskStatus::Blocked:
                return requested == TaskStatus::Todo || requested == TaskStatus::InProgress || requested == TaskStatus::Cancelled;
            case TaskStatus::Done:
            case TaskStatus::Cancelled:
                return requested == TaskStatus::Todo;
            default:
                return false;
            }
        }

        bool _isAgentTransitionAllowed(const TaskStatus current, const TaskStatus requested) noexcept
        {
            switch (requested)
            {
            case TaskStatus::InProgress:
                return current == TaskStatus::Backlog || current == TaskStatus::Todo || current == TaskStatus::Blocked || current == TaskStatus::InReview;
            case TaskStatus::InReview:
            case TaskStatus::Blocked:
                return current == TaskStatus::InProgress;
            default:
                return false;
            }
        }
    }

    StateTransitionDecision<TaskStatus> EvaluateTaskStatusTransition(
        const TaskStatus current,
        const TaskStatus requested,
        const TaskStatusAuthority authority,
        const TaskStatusTransitionContext& context) noexcept
    {
        if (current == requested)
        {
            return StateTransitionDecision<TaskStatus>::Permit(current, false);
        }

        bool authorityAllows{};
        switch (authority)
        {
        case TaskStatusAuthority::User:
            authorityAllows = _isUserTransitionAllowed(current, requested);
            break;
        case TaskStatusAuthority::Agent:
            authorityAllows = _isAgentTransitionAllowed(current, requested);
            break;
        case TaskStatusAuthority::SystemRecovery:
            authorityAllows = current == TaskStatus::InProgress && requested == TaskStatus::Todo;
            break;
        }
        if (!authorityAllows)
        {
            const auto transitionExists = _isUserTransitionAllowed(current, requested);
            return StateTransitionDecision<TaskStatus>::Reject(
                current,
                transitionExists ? TransitionError::AuthorityDenied : TransitionError::InvalidTransition);
        }

        if ((requested == TaskStatus::Done || requested == TaskStatus::Cancelled) && context.HasActiveRun)
        {
            return StateTransitionDecision<TaskStatus>::Reject(current, TransitionError::ActiveRunConflict);
        }
        if (requested == TaskStatus::Done && context.HasUnclaimedMessages)
        {
            return StateTransitionDecision<TaskStatus>::Reject(current, TransitionError::UnclaimedMessages);
        }
        if (requested == TaskStatus::Done && context.HasPendingAutomaticRetry)
        {
            return StateTransitionDecision<TaskStatus>::Reject(current, TransitionError::AutomaticRetryPending);
        }
        if (authority == TaskStatusAuthority::SystemRecovery)
        {
            if (context.HasActiveRun)
            {
                return StateTransitionDecision<TaskStatus>::Reject(current, TransitionError::ActiveRunConflict);
            }
            if (context.HasPendingAutomaticRetry)
            {
                return StateTransitionDecision<TaskStatus>::Reject(current, TransitionError::AutomaticRetryPending);
            }
        }

        return StateTransitionDecision<TaskStatus>::Permit(requested);
    }
}
