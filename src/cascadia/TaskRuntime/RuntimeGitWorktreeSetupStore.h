// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeGitWorktreeSetup.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    [[nodiscard]] Microsoft::Terminal::TaskCore::TaskStoreCommandResult
    AdvanceRuntimeGitWorktreeSetup(
        Microsoft::Terminal::TaskCore::ITaskStore& store,
        const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate,
        const RuntimeGitWorktreeSetupContext& context,
        Microsoft::Terminal::TaskCore::TaskGitWorktreeState requestedState,
        std::string snapshotDigest = {},
        std::string baselineCommit = {},
        bool hasUndeliveredWork = false,
        std::string preservedPath = {},
        std::string failureReasonCode = {});
}
