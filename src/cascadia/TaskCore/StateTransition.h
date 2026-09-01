// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

namespace Microsoft::Terminal::TaskCore
{
    enum class TransitionError
    {
        None,
        InvalidTransition,
        AuthorityDenied,
        ActiveRunConflict,
        UnclaimedMessages,
        AutomaticRetryPending,
        ExplicitOpenRequired,
        GenerationMismatch,
        ManualHoldActive,
        ActiveIntentConflict,
        IntentRevisionMismatch,
        GitWorktreeRevisionMismatch,
        GitCheckpointRevisionMismatch,
        MessageRevisionMismatch,
        UndeliveredWork,
    };

    template<typename TState>
    struct StateTransitionDecision
    {
        bool Allowed{};
        bool Changed{};
        TState NextState{};
        TransitionError Error{ TransitionError::InvalidTransition };

        [[nodiscard]] static StateTransitionDecision Permit(const TState nextState, const bool changed = true) noexcept
        {
            return { true, changed, nextState, TransitionError::None };
        }

        [[nodiscard]] static StateTransitionDecision Reject(const TState currentState, const TransitionError error) noexcept
        {
            return { false, false, currentState, error };
        }
    };
}
