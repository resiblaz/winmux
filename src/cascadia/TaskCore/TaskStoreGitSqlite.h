// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRecords.h"

#include <winsqlite/winsqlite3.h>

#include <string>

namespace Microsoft::Terminal::TaskCore::details
{
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
    [[nodiscard]] bool HasBlockingGitDelivery(
        sqlite3* database,
        const TaskId& task,
        bool& blocked,
        std::string& error);
}
