// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskIntentStatePolicy.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;

namespace TaskCoreLocalTests
{
    class TaskCoreIntentPolicyTests
    {
        TEST_CLASS(TaskCoreIntentPolicyTests);

        TEST_METHOD(RestrictsStagesByIntentKind);
        TEST_METHOD(AllowsForwardProgressFailureAndRetry);
        TEST_METHOD(RejectsBackwardAndCompletedTransitions);
        TEST_METHOD(IdentifiesManualHoldIntentKinds);
    };

    void TaskCoreIntentPolicyTests::RestrictsStagesByIntentKind()
    {
        VERIFY_IS_TRUE(IsTaskIntentStageSupported(TaskIntentKind::Stop, TaskIntentStage::Interrupting));
        VERIFY_IS_FALSE(IsTaskIntentStageSupported(TaskIntentKind::Stop, TaskIntentStage::Checkpointing));
        VERIFY_IS_TRUE(IsTaskIntentStageSupported(TaskIntentKind::Close, TaskIntentStage::RemovingSurface));
        VERIFY_IS_FALSE(IsTaskIntentStageSupported(TaskIntentKind::Close, TaskIntentStage::FinalizingWorktree));
        VERIFY_IS_TRUE(IsTaskIntentStageSupported(TaskIntentKind::WorktreeCleanup, TaskIntentStage::FinalizingWorktree));
        VERIFY_IS_FALSE(IsTaskIntentStageSupported(TaskIntentKind::WorktreeCleanup, TaskIntentStage::Interrupting));
        VERIFY_IS_TRUE(IsTaskIntentStageSupported(TaskIntentKind::Delete, TaskIntentStage::DeletingAggregate));
    }

    void TaskCoreIntentPolicyTests::AllowsForwardProgressFailureAndRetry()
    {
        auto decision = EvaluateTaskIntentTransition(
            TaskIntentKind::Delete,
            TaskIntentStage::Requested,
            TaskIntentStage::Checkpointing);
        VERIFY_IS_TRUE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Changed);

        decision = EvaluateTaskIntentTransition(
            TaskIntentKind::Delete,
            TaskIntentStage::Checkpointing,
            TaskIntentStage::Failed);
        VERIFY_IS_TRUE(decision.Allowed);

        decision = EvaluateTaskIntentTransition(
            TaskIntentKind::Delete,
            TaskIntentStage::Failed,
            TaskIntentStage::Requested);
        VERIFY_IS_TRUE(decision.Allowed);

        decision = EvaluateTaskIntentTransition(
            TaskIntentKind::Close,
            TaskIntentStage::Requested,
            TaskIntentStage::Completed);
        VERIFY_IS_TRUE(decision.Allowed);
    }

    void TaskCoreIntentPolicyTests::RejectsBackwardAndCompletedTransitions()
    {
        auto decision = EvaluateTaskIntentTransition(
            TaskIntentKind::Delete,
            TaskIntentStage::Checkpointing,
            TaskIntentStage::Interrupting);
        VERIFY_IS_FALSE(decision.Allowed);
        VERIFY_IS_TRUE(decision.Error == TransitionError::InvalidTransition);

        decision = EvaluateTaskIntentTransition(
            TaskIntentKind::Close,
            TaskIntentStage::Completed,
            TaskIntentStage::Requested);
        VERIFY_IS_FALSE(decision.Allowed);

        decision = EvaluateTaskIntentTransition(
            TaskIntentKind::Stop,
            TaskIntentStage::Interrupting,
            TaskIntentStage::Checkpointing);
        VERIFY_IS_FALSE(decision.Allowed);

        decision = EvaluateTaskIntentTransition(
            TaskIntentKind::Stop,
            TaskIntentStage::Interrupting,
            TaskIntentStage::Interrupting);
        VERIFY_IS_TRUE(decision.Allowed);
        VERIFY_IS_FALSE(decision.Changed);

        decision = EvaluateTaskIntentTransition(
            TaskIntentKind::Delete,
            TaskIntentStage::DeletingAggregate,
            TaskIntentStage::Completed);
        VERIFY_IS_FALSE(decision.Allowed);
    }

    void TaskCoreIntentPolicyTests::IdentifiesManualHoldIntentKinds()
    {
        VERIFY_IS_TRUE(TaskIntentSetsManualHold(TaskIntentKind::Stop));
        VERIFY_IS_TRUE(TaskIntentSetsManualHold(TaskIntentKind::Close));
        VERIFY_IS_TRUE(TaskIntentSetsManualHold(TaskIntentKind::Delete));
        VERIFY_IS_FALSE(TaskIntentSetsManualHold(TaskIntentKind::WorktreeCleanup));
    }
}
