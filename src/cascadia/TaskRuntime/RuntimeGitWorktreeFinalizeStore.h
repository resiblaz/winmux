// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeGitWorktreeFinalize.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    [[nodiscard]] Microsoft::Terminal::TaskCore::TaskStoreCommandResult
    AdvanceRuntimeGitWorktreeFinalization(
        Microsoft::Terminal::TaskCore::ITaskStore& store,
        const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate,
        const RuntimeGitWorktreeFinalizeContext& context,
        Microsoft::Terminal::TaskCore::TaskGitWorktreeState requestedState,
        bool hasUndeliveredWork,
        bool cleanupEligible,
        std::string preservedPath = {},
        std::string failureReasonCode = {});
}
