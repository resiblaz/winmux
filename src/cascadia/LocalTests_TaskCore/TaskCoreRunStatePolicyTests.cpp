// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRunStatePolicy.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;

namespace TaskCoreLocalTests
{
    class TaskCoreRunStatePolicyTests
    {
        TEST_CLASS(TaskCoreRunStatePolicyTests);

        TEST_METHOD(AcceptsOrderedExecutionAndTerminalOutcomes);
        TEST_METHOD(RejectsSkippedAndBackwardTransitions);
        TEST_METHOD(TerminalOutcomesAreImmutable);
    };

    void TaskCoreRunStatePolicyTests::AcceptsOrderedExecutionAndTerminalOutcomes()
    {
        for (const auto [from, to] : {
                 std::pair{ TaskRunState::Queued, TaskRunState::Preparing },
                 std::pair{ TaskRunState::Preparing, TaskRunState::WaitingForDirectory },
                 std::pair{ TaskRunState::WaitingForDirectory, TaskRunState::Starting },
                 std::pair{ TaskRunState::Starting, TaskRunState::Running },
                 std::pair{ TaskRunState::Running, TaskRunState::Completed },
             })
        {
            VERIFY_IS_TRUE(EvaluateTaskRunTransition(from, to).Allowed);
        }
        for (const auto outcome : { TaskRunState::Completed, TaskRunState::Failed, TaskRunState::Cancelled, TaskRunState::Interrupted })
        {
            VERIFY_IS_TRUE(IsTaskRunTerminal(outcome));
            VERIFY_IS_TRUE(EvaluateTaskRunTransition(TaskRunState::Running, outcome).Allowed);
        }
    }

    void TaskCoreRunStatePolicyTests::RejectsSkippedAndBackwardTransitions()
    {
        VERIFY_IS_FALSE(EvaluateTaskRunTransition(TaskRunState::Queued, TaskRunState::Running).Allowed);
        VERIFY_IS_FALSE(EvaluateTaskRunTransition(TaskRunState::Running, TaskRunState::Preparing).Allowed);
        VERIFY_IS_FALSE(EvaluateTaskRunTransition(TaskRunState::WaitingForDirectory, TaskRunState::Preparing).Allowed);
        VERIFY_IS_TRUE(IsTaskRunActive(TaskRunState::WaitingForDirectory));
    }

    void TaskCoreRunStatePolicyTests::TerminalOutcomesAreImmutable()
    {
        for (const auto terminal : { TaskRunState::Completed, TaskRunState::Failed, TaskRunState::Cancelled, TaskRunState::Interrupted })
        {
            for (const auto requested : { TaskRunState::Queued, TaskRunState::Running, TaskRunState::Completed, TaskRunState::Failed })
            {
                const auto decision = EvaluateTaskRunTransition(terminal, requested);
                if (terminal == requested)
                {
                    VERIFY_IS_TRUE(decision.Allowed);
                    VERIFY_IS_FALSE(decision.Changed);
                }
                else
                {
                    VERIFY_IS_FALSE(decision.Allowed);
                }
            }
        }
    }
}
