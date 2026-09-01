// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeGitWorktreeCheckpoint.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    [[nodiscard]] Microsoft::Terminal::TaskCore::TaskStoreCommandResult
    BeginRuntimeGitWorktreeCheckpoint(
        Microsoft::Terminal::TaskCore::ITaskStore& store,
        const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate,
        const Microsoft::Terminal::TaskCore::TaskRunRecord& run,
        const RuntimeGitWorktreeCheckpointContext& context,
        std::string startCommit,
        std::string startSummaryJson);

    [[nodiscard]] Microsoft::Terminal::TaskCore::TaskStoreCommandResult
    AdvanceRuntimeGitWorktreeCheckpoint(
        Microsoft::Terminal::TaskCore::ITaskStore& store,
        const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate,
        const Microsoft::Terminal::TaskCore::TaskRunGitCheckpointRecord& checkpoint,
        const RuntimeGitWorktreeCheckpointContext& context,
        Microsoft::Terminal::TaskCore::TaskGitCheckpointState requestedState,
        std::string endCommit,
        std::string checkpointCommit,
        bool autoCheckpointed,
        std::string changedPathsJson,
        std::string diffSummaryJson,
        bool hasUndeliveredWork,
        std::string preservedPath = {},
        std::string reasonCode = {});
}
