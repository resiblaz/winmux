// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreCommandTypes.h"

#include <winsqlite/winsqlite3.h>

#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskCore::details
{
    [[nodiscard]] bool LoadTaskRunGitCheckpoint(
        sqlite3* database,
        const GitCheckpointId& checkpoint,
        std::optional<TaskRunGitCheckpointRecord>& record,
        std::string& error);
    [[nodiscard]] bool TaskRunHasGitCheckpoint(
        sqlite3* database,
        const TaskRunId& run,
        bool& exists,
        std::string& error);
    [[nodiscard]] bool TaskHasIncompleteGitCheckpoint(
        sqlite3* database,
        const TaskId& task,
        bool& exists,
        std::string& error);
    [[nodiscard]] bool TaskRunUsesGitWorktree(
        sqlite3* database,
        const TaskId& task,
        const TaskRunId& run,
        const ExecutionContextId& context,
        bool& matches,
        std::string& error);
    [[nodiscard]] bool InsertTaskRunGitCheckpoint(
        sqlite3* database,
        const TaskRunGitCheckpointRecord& checkpoint,
        std::string& error);
    [[nodiscard]] bool AdvanceTaskRunGitCheckpoint(
        sqlite3* database,
        const AdvanceTaskRunGitCheckpointCommand& command,
        TaskGitCheckpointState currentState,
        uint64_t committedRevision,
        std::string& error);
    [[nodiscard]] bool AdvanceGitWorktreeForCheckpoint(
        sqlite3* database,
        const AdvanceTaskRunGitCheckpointCommand& command,
        uint64_t committedRevision,
        std::string& error);
}
