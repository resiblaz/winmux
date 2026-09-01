// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreCommandTypes.h"

#include <winsqlite/winsqlite3.h>

#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskCore::details
{
    [[nodiscard]] bool LoadTaskGitWorktree(
        sqlite3* database,
        const TaskId& task,
        std::optional<TaskGitWorktreeRecord>& worktree,
        std::string& error);
    [[nodiscard]] bool ValidateGitWorktreeExecutionContext(
        sqlite3* database,
        const TaskGitWorktreeRecord& worktree,
        bool& matches,
        std::string& error);
    [[nodiscard]] bool TaskHasGitWorktree(
        sqlite3* database,
        const TaskId& task,
        bool& exists,
        std::string& error);
    [[nodiscard]] bool InsertTaskGitWorktree(
        sqlite3* database,
        const TaskGitWorktreeRecord& worktree,
        std::string& error);
    [[nodiscard]] bool AdvanceTaskGitWorktree(
        sqlite3* database,
        const AdvanceTaskGitWorktreeCommand& command,
        TaskGitWorktreeState currentState,
        uint64_t committedRevision,
        std::string& error);
    [[nodiscard]] bool AdvanceTaskGitWorktreeFinalization(
        sqlite3* database,
        const AdvanceTaskGitWorktreeFinalizationCommand& command,
        TaskGitWorktreeState currentState,
        uint64_t committedRevision,
        std::string& error);
    [[nodiscard]] bool TaskGitWorktreeBlocksRun(
        sqlite3* database,
        const TaskId& task,
        bool& blocked,
        std::string& error);
    [[nodiscard]] bool HasBlockingGitDelivery(
        sqlite3* database,
        const TaskId& task,
        bool& blocked,
        std::string& error);
}
