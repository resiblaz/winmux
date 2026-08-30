// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskStatusPolicy.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;

namespace TaskCoreLocalTests
{
    class TaskCoreTaskStatusPolicyTests
    {
        TEST_CLASS(TaskCoreTaskStatusPolicyTests);

        TEST_METHOD(UserTransitionsFollowProductFlow);
        TEST_METHOD(AgentAuthorityCannotCompleteOrCancel);
        TEST_METHOD(CompletionCancellationAndRecoveryRespectGuards);
    };

    void TaskCoreTaskStatusPolicyTests::UserTransitionsFollowProductFlow()
    {
        const TaskStatusTransitionContext context{};
        for (const auto [from, to] : {
                 std::pair{ TaskStatus::Backlog, TaskStatus::Todo },
                 std::pair{ TaskStatus::Todo, TaskStatus::InProgress },
                 std::pair{ TaskStatus::InProgress, TaskStatus::InReview },
                 std::pair{ TaskStatus::InProgress, TaskStatus::Blocked },
                 std::pair{ TaskStatus::Blocked, TaskStatus::InProgress },
                 std::pair{ TaskStatus::InReview, TaskStatus::Done },
                 std::pair{ TaskStatus::Done, TaskStatus::Todo },
                 std::pair{ TaskStatus::Cancelled, TaskStatus::Todo },
             })
        {
            const auto decision = EvaluateTaskStatusTransition(from, to, TaskStatusAuthority::User, context);
            VERIFY_IS_TRUE(decision.Allowed);
            VERIFY_IS_TRUE(decision.Changed);
            VERIFY_IS_TRUE(decision.NextState == to);
        }

        const auto shortcut = EvaluateTaskStatusTransition(TaskStatus::Todo, TaskStatus::Done, TaskStatusAuthority::User, context);
        VERIFY_IS_FALSE(shortcut.Allowed);
        VERIFY_IS_TRUE(shortcut.Error == TransitionError::InvalidTransition);
    }

    void TaskCoreTaskStatusPolicyTests::AgentAuthorityCannotCompleteOrCancel()
    {
        const TaskStatusTransitionContext context{};
        VERIFY_IS_TRUE(EvaluateTaskStatusTransition(TaskStatus::Todo, TaskStatus::InProgress, TaskStatusAuthority::Agent, context).Allowed);
        VERIFY_IS_TRUE(EvaluateTaskStatusTransition(TaskStatus::InProgress, TaskStatus::InReview, TaskStatusAuthority::Agent, context).Allowed);
        VERIFY_IS_TRUE(EvaluateTaskStatusTransition(TaskStatus::InProgress, TaskStatus::Blocked, TaskStatusAuthority::Agent, context).Allowed);

        const auto done = EvaluateTaskStatusTransition(TaskStatus::InReview, TaskStatus::Done, TaskStatusAuthority::Agent, context);
        const auto cancelled = EvaluateTaskStatusTransition(TaskStatus::InProgress, TaskStatus::Cancelled, TaskStatusAuthority::Agent, context);
        VERIFY_IS_FALSE(done.Allowed);
        VERIFY_IS_TRUE(done.Error == TransitionError::AuthorityDenied);
        VERIFY_IS_FALSE(cancelled.Allowed);
        VERIFY_IS_TRUE(cancelled.Error == TransitionError::AuthorityDenied);
    }

    void TaskCoreTaskStatusPolicyTests::CompletionCancellationAndRecoveryRespectGuards()
    {
        TaskStatusTransitionContext context;
        context.HasActiveRun = true;
        auto decision = EvaluateTaskStatusTransition(TaskStatus::InReview, TaskStatus::Done, TaskStatusAuthority::User, context);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::ActiveRunConflict);

        context = {};
        context.HasUnclaimedMessages = true;
        decision = EvaluateTaskStatusTransition(TaskStatus::InReview, TaskStatus::Done, TaskStatusAuthority::User, context);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::UnclaimedMessages);

        context = {};
        context.HasPendingAutomaticRetry = true;
        decision = EvaluateTaskStatusTransition(TaskStatus::InProgress, TaskStatus::Todo, TaskStatusAuthority::SystemRecovery, context);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::AutomaticRetryPending);

        context = {};
        VERIFY_IS_TRUE(EvaluateTaskStatusTransition(TaskStatus::InProgress, TaskStatus::Todo, TaskStatusAuthority::SystemRecovery, context).Allowed);
        decision = EvaluateTaskStatusTransition(TaskStatus::InProgress, TaskStatus::Cancelled, TaskStatusAuthority::User, { .HasActiveRun = true });
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::ActiveRunConflict);
    }
}
