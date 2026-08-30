// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "StateTransition.h"
#include "TaskEnums.h"

namespace Microsoft::Terminal::TaskCore
{
    [[nodiscard]] bool IsTaskIntentStageSupported(TaskIntentKind kind, TaskIntentStage stage) noexcept;
    [[nodiscard]] bool TaskIntentSetsManualHold(TaskIntentKind kind) noexcept;
    [[nodiscard]] StateTransitionDecision<TaskIntentStage> EvaluateTaskIntentTransition(
        TaskIntentKind kind,
        TaskIntentStage current,
        TaskIntentStage requested) noexcept;
}
