// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreGitSqlite.h"

#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    namespace
    {
        bool _readExists(
            sqlite3* database,
            sqlite3_stmt* statement,
            bool& value,
            std::string& error)
        {
            const auto step = sqlite3_step(statement);
            if (step != SQLITE_ROW)
            {
                error = SqliteError(database, step, "query Task Git state");
                return false;
            }
            value = sqlite3_column_int(statement, 0) != 0;
            return true;
        }
    }

    bool ValidateGitWorktreeExecutionContext(
        sqlite3* database,
        const TaskGitWorktreeRecord& worktree,
        bool& matches,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT EXISTS(
    SELECT 1 FROM execution_context
    WHERE id=?1 AND task_id=?2 AND mode=1 AND identity_frozen=1
      AND repository_common_directory=?3 AND worktree_path=?4 AND branch=?5
);
)SQL";
        SqliteStatement statement{ database, sql, error };
        return statement &&
               BindText(database, statement.Get(), 1, worktree.ExecutionContext.Value(), error) &&
               BindText(database, statement.Get(), 2, worktree.Task.Value(), error) &&
               BindText(database, statement.Get(), 3, worktree.RepositoryCommonDirectory, error) &&
               BindText(database, statement.Get(), 4, worktree.WorktreePath, error) &&
               BindText(database, statement.Get(), 5, worktree.BranchRef, error) &&
               _readExists(database, statement.Get(), matches, error);
    }

    bool TaskHasGitWorktree(
        sqlite3* database,
        const TaskId& task,
        bool& exists,
        std::string& error)
    {
        SqliteStatement statement{
            database,
            "SELECT EXISTS(SELECT 1 FROM task_git_worktree WHERE task_id=?1);",
            error
        };
        return statement &&
               BindText(database, statement.Get(), 1, task.Value(), error) &&
               _readExists(database, statement.Get(), exists, error);
    }

    bool InsertTaskGitWorktree(
        sqlite3* database,
        const TaskGitWorktreeRecord& worktree,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
INSERT INTO task_git_worktree(
    task_id, execution_context_id, revision, state, repository_root,
    repository_common_directory, repository_identity_digest, worktree_path,
    branch_ref, source_head, source_reference, snapshot_digest, baseline_commit,
    ownership_nonce, has_undelivered_work, cleanup_eligible, preserved_path,
    failure_reason_code, created_at, updated_at)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20);
)SQL";
        SqliteStatement statement{ database, sql, error };
        return statement &&
               BindText(database, statement.Get(), 1, worktree.Task.Value(), error) &&
               BindText(database, statement.Get(), 2, worktree.ExecutionContext.Value(), error) &&
               BindInt64(database, statement.Get(), 3, static_cast<int64_t>(worktree.Revision), error) &&
               BindInt64(database, statement.Get(), 4, static_cast<int64_t>(worktree.State), error) &&
               BindText(database, statement.Get(), 5, worktree.RepositoryRoot, error) &&
               BindText(database, statement.Get(), 6, worktree.RepositoryCommonDirectory, error) &&
               BindText(database, statement.Get(), 7, worktree.RepositoryIdentityDigest, error) &&
               BindText(database, statement.Get(), 8, worktree.WorktreePath, error) &&
               BindText(database, statement.Get(), 9, worktree.BranchRef, error) &&
               BindText(database, statement.Get(), 10, worktree.SourceHead, error) &&
               BindText(database, statement.Get(), 11, worktree.SourceReference, error) &&
               BindText(database, statement.Get(), 12, worktree.SnapshotDigest, error) &&
               BindText(database, statement.Get(), 13, worktree.BaselineCommit, error) &&
               BindText(database, statement.Get(), 14, worktree.OwnershipNonce, error) &&
               BindInt64(database, statement.Get(), 15, worktree.HasUndeliveredWork ? 1 : 0, error) &&
               BindInt64(database, statement.Get(), 16, worktree.CleanupEligible ? 1 : 0, error) &&
               BindText(database, statement.Get(), 17, worktree.PreservedPath, error) &&
               BindText(database, statement.Get(), 18, worktree.FailureReasonCode, error) &&
               BindInt64(database, statement.Get(), 19, worktree.CreatedAtUnixMilliseconds, error) &&
               BindInt64(database, statement.Get(), 20, worktree.UpdatedAtUnixMilliseconds, error) &&
               StepDone(database, statement.Get(), error);
    }

    bool HasBlockingGitDelivery(
        sqlite3* database,
        const TaskId& task,
        bool& blocked,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT EXISTS(
    SELECT 1 FROM task_git_worktree
    WHERE task_id=?1 AND (
        state<>8 OR has_undelivered_work=1 OR cleanup_eligible=0
    )
);
)SQL";
        SqliteStatement statement{ database, sql, error };
        return statement &&
               BindText(database, statement.Get(), 1, task.Value(), error) &&
               _readExists(database, statement.Get(), blocked, error);
    }
}
