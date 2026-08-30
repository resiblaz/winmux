// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskIntentStatePolicy.h"

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        int _stageOrder(const TaskIntentStage stage) noexcept
        {
            switch (stage)
            {
            case TaskIntentStage::Requested:
                return 0;
            case TaskIntentStage::Interrupting:
                return 1;
            case TaskIntentStage::WaitingForProvider:
                return 2;
            case TaskIntentStage::Checkpointing:
                return 3;
            case TaskIntentStage::FinalizingWorktree:
                return 4;
            case TaskIntentStage::RemovingSurface:
                return 5;
            case TaskIntentStage::DeletingAggregate:
                return 6;
            case TaskIntentStage::Failed:
                return 7;
            case TaskIntentStage::Completed:
                return 8;
            default:
                return -1;
            }
        }
    }

    bool IsTaskIntentStageSupported(const TaskIntentKind kind, const TaskIntentStage stage) noexcept
    {
        switch (kind)
        {
        case TaskIntentKind::Stop:
            return stage == TaskIntentStage::Requested ||
                   stage == TaskIntentStage::Interrupting ||
                   stage == TaskIntentStage::WaitingForProvider ||
                   stage == TaskIntentStage::Failed ||
                   stage == TaskIntentStage::Completed;
        case TaskIntentKind::Close:
            return stage == TaskIntentStage::Requested ||
                   stage == TaskIntentStage::Interrupting ||
                   stage == TaskIntentStage::WaitingForProvider ||
                   stage == TaskIntentStage::Checkpointing ||
                   stage == TaskIntentStage::RemovingSurface ||
                   stage == TaskIntentStage::Failed ||
                   stage == TaskIntentStage::Completed;
        case TaskIntentKind::WorktreeCleanup:
            return stage == TaskIntentStage::Requested ||
                   stage == TaskIntentStage::Checkpointing ||
                   stage == TaskIntentStage::FinalizingWorktree ||
                   stage == TaskIntentStage::Failed ||
                   stage == TaskIntentStage::Completed;
        case TaskIntentKind::Delete:
            return _stageOrder(stage) >= 0;
        default:
            return false;
        }
    }

    bool TaskIntentSetsManualHold(const TaskIntentKind kind) noexcept
    {
        return kind == TaskIntentKind::Stop ||
               kind == TaskIntentKind::Close ||
               kind == TaskIntentKind::Delete;
    }

    StateTransitionDecision<TaskIntentStage> EvaluateTaskIntentTransition(
        const TaskIntentKind kind,
        const TaskIntentStage current,
        const TaskIntentStage requested) noexcept
    {
        if (!IsTaskIntentStageSupported(kind, current) || !IsTaskIntentStageSupported(kind, requested))
        {
            return StateTransitionDecision<TaskIntentStage>::Reject(current, TransitionError::InvalidTransition);
        }
        if (kind == TaskIntentKind::Delete && requested == TaskIntentStage::Completed)
        {
            return StateTransitionDecision<TaskIntentStage>::Reject(current, TransitionError::InvalidTransition);
        }
        if (current == requested)
        {
            return StateTransitionDecision<TaskIntentStage>::Permit(current, false);
        }
        if (current == TaskIntentStage::Completed)
        {
            return StateTransitionDecision<TaskIntentStage>::Reject(current, TransitionError::InvalidTransition);
        }
        if (requested == TaskIntentStage::Failed || requested == TaskIntentStage::Completed)
        {
            return StateTransitionDecision<TaskIntentStage>::Permit(requested);
        }
        if (current == TaskIntentStage::Failed)
        {
            return requested == TaskIntentStage::Requested ?
                       StateTransitionDecision<TaskIntentStage>::Permit(requested) :
                       StateTransitionDecision<TaskIntentStage>::Reject(current, TransitionError::InvalidTransition);
        }

        return _stageOrder(requested) > _stageOrder(current) ?
                   StateTransitionDecision<TaskIntentStage>::Permit(requested) :
                   StateTransitionDecision<TaskIntentStage>::Reject(current, TransitionError::InvalidTransition);
    }
}
