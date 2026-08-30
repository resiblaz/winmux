// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskTerminalStatePolicy.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;

namespace TaskCoreLocalTests
{
    class TaskCoreTerminalStatePolicyTests
    {
        TEST_CLASS(TaskCoreTerminalStatePolicyTests);

        TEST_METHOD(NewAndReopenedTerminalsRequireFreshGeneration);
        TEST_METHOD(InGenerationTransitionsKeepGeneration);
        TEST_METHOD(ActiveRunBlocksSuspendAndClose);
    };

    void TaskCoreTerminalStatePolicyTests::NewAndReopenedTerminalsRequireFreshGeneration()
    {
        TaskTerminalTransitionContext context{ .CurrentGeneration = 0, .RequestedGeneration = 1, .ExplicitOpen = true };
        VERIFY_IS_TRUE(EvaluateTaskTerminalTransition(TaskTerminalState::NotCreated, TaskTerminalState::Starting, context).Allowed);

        context.ExplicitOpen = false;
        auto decision = EvaluateTaskTerminalTransition(TaskTerminalState::Closed, TaskTerminalState::Starting, context);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::ExplicitOpenRequired);

        context = { .CurrentGeneration = 4, .RequestedGeneration = 4, .ExplicitOpen = true };
        decision = EvaluateTaskTerminalTransition(TaskTerminalState::Suspended, TaskTerminalState::Starting, context);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::GenerationMismatch);

        context.RequestedGeneration = 5;
        VERIFY_IS_TRUE(EvaluateTaskTerminalTransition(TaskTerminalState::Suspended, TaskTerminalState::Starting, context).Allowed);
    }

    void TaskCoreTerminalStatePolicyTests::InGenerationTransitionsKeepGeneration()
    {
        const TaskTerminalTransitionContext context{ .CurrentGeneration = 3, .RequestedGeneration = 3 };
        VERIFY_IS_TRUE(EvaluateTaskTerminalTransition(TaskTerminalState::Starting, TaskTerminalState::Ready, context).Allowed);
        VERIFY_IS_TRUE(EvaluateTaskTerminalTransition(TaskTerminalState::Ready, TaskTerminalState::Busy, context).Allowed);
        VERIFY_IS_TRUE(EvaluateTaskTerminalTransition(TaskTerminalState::Busy, TaskTerminalState::Ready, context).Allowed);

        auto stale = context;
        stale.RequestedGeneration = 2;
        auto decision = EvaluateTaskTerminalTransition(TaskTerminalState::Ready, TaskTerminalState::Busy, stale);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::GenerationMismatch);

        decision = EvaluateTaskTerminalTransition(TaskTerminalState::Ready, TaskTerminalState::Ready, stale);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::GenerationMismatch);
    }

    void TaskCoreTerminalStatePolicyTests::ActiveRunBlocksSuspendAndClose()
    {
        const TaskTerminalTransitionContext context{
            .CurrentGeneration = 2,
            .RequestedGeneration = 2,
            .HasActiveRun = true,
        };
        auto decision = EvaluateTaskTerminalTransition(TaskTerminalState::Ready, TaskTerminalState::Suspended, context);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::ActiveRunConflict);

        decision = EvaluateTaskTerminalTransition(TaskTerminalState::Busy, TaskTerminalState::Closed, context);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::ActiveRunConflict);
        VERIFY_IS_FALSE(EvaluateTaskTerminalTransition(TaskTerminalState::Busy, TaskTerminalState::Suspended, context).Allowed);
    }
}
