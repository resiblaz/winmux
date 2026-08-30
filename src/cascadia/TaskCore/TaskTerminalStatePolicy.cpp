// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskTerminalStatePolicy.h"

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        bool _startsNewGeneration(const TaskTerminalState current, const TaskTerminalState requested) noexcept
        {
            return requested == TaskTerminalState::Starting &&
                   (current == TaskTerminalState::NotCreated ||
                    current == TaskTerminalState::Exited ||
                    current == TaskTerminalState::Suspended ||
                    current == TaskTerminalState::Error ||
                    current == TaskTerminalState::Closed);
        }

        bool _isTransitionAllowed(const TaskTerminalState current, const TaskTerminalState requested) noexcept
        {
            switch (current)
            {
            case TaskTerminalState::NotCreated:
                return requested == TaskTerminalState::Starting || requested == TaskTerminalState::Closed;
            case TaskTerminalState::Starting:
                return requested == TaskTerminalState::Ready || requested == TaskTerminalState::Busy ||
                       requested == TaskTerminalState::Exited || requested == TaskTerminalState::Error || requested == TaskTerminalState::Closed;
            case TaskTerminalState::Ready:
                return requested == TaskTerminalState::Busy || requested == TaskTerminalState::Exited ||
                       requested == TaskTerminalState::Suspended || requested == TaskTerminalState::Error || requested == TaskTerminalState::Closed;
            case TaskTerminalState::Busy:
                return requested == TaskTerminalState::Ready || requested == TaskTerminalState::Exited ||
                       requested == TaskTerminalState::Error || requested == TaskTerminalState::Closed;
            case TaskTerminalState::Exited:
            case TaskTerminalState::Suspended:
            case TaskTerminalState::Error:
                return requested == TaskTerminalState::Starting || requested == TaskTerminalState::Closed;
            case TaskTerminalState::Closed:
                return requested == TaskTerminalState::Starting;
            default:
                return false;
            }
        }
    }

    StateTransitionDecision<TaskTerminalState> EvaluateTaskTerminalTransition(
        const TaskTerminalState current,
        const TaskTerminalState requested,
        const TaskTerminalTransitionContext& context) noexcept
    {
        if (current == requested)
        {
            if (context.RequestedGeneration != context.CurrentGeneration)
            {
                return StateTransitionDecision<TaskTerminalState>::Reject(current, TransitionError::GenerationMismatch);
            }
            return StateTransitionDecision<TaskTerminalState>::Permit(current, false);
        }
        if (!_isTransitionAllowed(current, requested))
        {
            return StateTransitionDecision<TaskTerminalState>::Reject(current, TransitionError::InvalidTransition);
        }
        if ((requested == TaskTerminalState::Closed || requested == TaskTerminalState::Suspended) && context.HasActiveRun)
        {
            return StateTransitionDecision<TaskTerminalState>::Reject(current, TransitionError::ActiveRunConflict);
        }

        if (_startsNewGeneration(current, requested))
        {
            if (!context.ExplicitOpen)
            {
                return StateTransitionDecision<TaskTerminalState>::Reject(current, TransitionError::ExplicitOpenRequired);
            }
            if (context.RequestedGeneration != context.CurrentGeneration + 1)
            {
                return StateTransitionDecision<TaskTerminalState>::Reject(current, TransitionError::GenerationMismatch);
            }
        }
        else if (context.RequestedGeneration != context.CurrentGeneration)
        {
            return StateTransitionDecision<TaskTerminalState>::Reject(current, TransitionError::GenerationMismatch);
        }

        return StateTransitionDecision<TaskTerminalState>::Permit(requested);
    }
}
