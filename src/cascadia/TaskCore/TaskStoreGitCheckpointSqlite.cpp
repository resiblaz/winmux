// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreGitCheckpointSqlite.h"

#include "TaskStoreRowSqlite.h"
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
                error = SqliteError(database, step, "query Task Git checkpoint");
                return false;
            }
            value = sqlite3_column_int(statement, 0) != 0;
            return true;
        }
    }

    bool LoadTaskRunGitCheckpoint(
        sqlite3* database,
        const GitCheckpointId& checkpoint,
        std::optional<TaskRunGitCheckpointRecord>& record,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT id, task_id, run_id, revision, state, start_commit, end_commit,
       checkpoint_commit, auto_checkpointed, changed_paths_json,
       diff_summary_json, preserved_path, reason_code, created_at, updated_at
FROM git_checkpoint WHERE id=?1;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement ||
            !BindText(database, statement.Get(), 1, checkpoint.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step == SQLITE_DONE)
        {
            record.reset();
            return true;
        }
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "read Task Git checkpoint");
            return false;
        }
        record = ReadTaskRunGitCheckpointRow(statement.Get());
        return true;
    }

    bool TaskRunHasGitCheckpoint(
        sqlite3* database,
        const TaskRunId& run,
        bool& exists,
        std::string& error)
    {
        SqliteStatement statement{
            database,
            "SELECT EXISTS(SELECT 1 FROM git_checkpoint WHERE run_id=?1);",
            error
        };
        return statement &&
               BindText(database, statement.Get(), 1, run.Value(), error) &&
               _readExists(database, statement.Get(), exists, error);
    }

    bool TaskHasIncompleteGitCheckpoint(
        sqlite3* database,
        const TaskId& task,
        bool& exists,
        std::string& error)
    {
        SqliteStatement statement{
            database,
            "SELECT EXISTS(SELECT 1 FROM git_checkpoint WHERE task_id=?1 AND state IN (0,1,2,5));",
            error
        };
        return statement &&
               BindText(database, statement.Get(), 1, task.Value(), error) &&
               _readExists(database, statement.Get(), exists, error);
    }

    bool TaskRunUsesGitWorktree(
        sqlite3* database,
        const TaskId& task,
        const TaskRunId& run,
        const ExecutionContextId& context,
        bool& matches,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT EXISTS(
    SELECT 1
    FROM task_run r
    JOIN execution_context e ON e.id=r.execution_context_id
    JOIN task_git_worktree w ON w.execution_context_id=e.id
    WHERE r.id=?1 AND r.task_id=?2 AND r.execution_context_id=?3
      AND e.mode=1 AND e.identity_frozen=1 AND w.task_id=r.task_id
);
)SQL";
        SqliteStatement statement{ database, sql, error };
        return statement &&
               BindText(database, statement.Get(), 1, run.Value(), error) &&
               BindText(database, statement.Get(), 2, task.Value(), error) &&
               BindText(database, statement.Get(), 3, context.Value(), error) &&
               _readExists(database, statement.Get(), matches, error);
    }

    bool InsertTaskRunGitCheckpoint(
        sqlite3* database,
        const TaskRunGitCheckpointRecord& checkpoint,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
INSERT INTO git_checkpoint(
    id, task_id, run_id, revision, state, start_commit, end_commit,
    checkpoint_commit, auto_checkpointed, changed_paths_json,
    diff_summary_json, preserved_path, reason_code, created_at, updated_at)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15);
)SQL";
        SqliteStatement statement{ database, sql, error };
        return statement &&
               BindText(database, statement.Get(), 1, checkpoint.Id.Value(), error) &&
               BindText(database, statement.Get(), 2, checkpoint.Task.Value(), error) &&
               BindText(database, statement.Get(), 3, checkpoint.Run.Value(), error) &&
               BindInt64(database, statement.Get(), 4, static_cast<int64_t>(checkpoint.Revision), error) &&
               BindInt64(database, statement.Get(), 5, static_cast<int64_t>(checkpoint.State), error) &&
               BindText(database, statement.Get(), 6, checkpoint.StartCommit, error) &&
               BindText(database, statement.Get(), 7, checkpoint.EndCommit, error) &&
               BindText(database, statement.Get(), 8, checkpoint.CheckpointCommit, error) &&
               BindInt64(database, statement.Get(), 9, checkpoint.AutoCheckpointed ? 1 : 0, error) &&
               BindText(database, statement.Get(), 10, checkpoint.ChangedPathsJson, error) &&
               BindText(database, statement.Get(), 11, checkpoint.DiffSummaryJson, error) &&
               BindText(database, statement.Get(), 12, checkpoint.PreservedPath, error) &&
               BindText(database, statement.Get(), 13, checkpoint.ReasonCode, error) &&
               BindInt64(database, statement.Get(), 14, checkpoint.CreatedAtUnixMilliseconds, error) &&
               BindInt64(database, statement.Get(), 15, checkpoint.UpdatedAtUnixMilliseconds, error) &&
               StepDone(database, statement.Get(), error);
    }

    bool AdvanceTaskRunGitCheckpoint(
        sqlite3* database,
        const AdvanceTaskRunGitCheckpointCommand& command,
        const TaskGitCheckpointState currentState,
        const uint64_t committedRevision,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
UPDATE git_checkpoint
SET revision=?1, state=?2, end_commit=?3, checkpoint_commit=?4,
    auto_checkpointed=?5, changed_paths_json=?6, diff_summary_json=?7,
    preserved_path=?8, reason_code=?9, updated_at=?10
WHERE id=?11 AND task_id=?12 AND run_id=?13 AND revision=?14 AND state=?15;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement ||
            !BindInt64(database, statement.Get(), 1, static_cast<int64_t>(committedRevision), error) ||
            !BindInt64(database, statement.Get(), 2, static_cast<int64_t>(command.RequestedState), error) ||
            !BindText(database, statement.Get(), 3, command.EndCommit, error) ||
            !BindText(database, statement.Get(), 4, command.CheckpointCommit, error) ||
            !BindInt64(database, statement.Get(), 5, command.AutoCheckpointed ? 1 : 0, error) ||
            !BindText(database, statement.Get(), 6, command.ChangedPathsJson, error) ||
            !BindText(database, statement.Get(), 7, command.DiffSummaryJson, error) ||
            !BindText(database, statement.Get(), 8, command.PreservedPath, error) ||
            !BindText(database, statement.Get(), 9, command.ReasonCode, error) ||
            !BindInt64(database, statement.Get(), 10, command.Metadata.TimestampUnixMilliseconds, error) ||
            !BindText(database, statement.Get(), 11, command.Checkpoint.Value(), error) ||
            !BindText(database, statement.Get(), 12, command.Task.Value(), error) ||
            !BindText(database, statement.Get(), 13, command.Run.Value(), error) ||
            !BindInt64(database, statement.Get(), 14, static_cast<int64_t>(command.ExpectedCheckpointRevision), error) ||
            !BindInt64(database, statement.Get(), 15, static_cast<int64_t>(currentState), error) ||
            !StepDone(database, statement.Get(), error))
        {
            return false;
        }
        if (sqlite3_changes(database) != 1)
        {
            error = "Task Git checkpoint changed during transaction";
            return false;
        }
        return true;
    }

    bool AdvanceGitWorktreeForCheckpoint(
        sqlite3* database,
        const AdvanceTaskRunGitCheckpointCommand& command,
        const uint64_t committedRevision,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
UPDATE task_git_worktree
SET revision=?1, has_undelivered_work=?2, cleanup_eligible=0,
    preserved_path=?3, failure_reason_code=?4, updated_at=?5
WHERE task_id=?6 AND revision=?7 AND state=5;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement ||
            !BindInt64(database, statement.Get(), 1, static_cast<int64_t>(committedRevision), error) ||
            !BindInt64(database, statement.Get(), 2, command.HasUndeliveredWork ? 1 : 0, error) ||
            !BindText(database, statement.Get(), 3, command.PreservedPath, error) ||
            !BindText(database, statement.Get(), 4, command.ReasonCode, error) ||
            !BindInt64(database, statement.Get(), 5, command.Metadata.TimestampUnixMilliseconds, error) ||
            !BindText(database, statement.Get(), 6, command.Task.Value(), error) ||
            !BindInt64(database, statement.Get(), 7, static_cast<int64_t>(command.ExpectedWorktreeRevision), error) ||
            !StepDone(database, statement.Get(), error))
        {
            return false;
        }
        if (sqlite3_changes(database) != 1)
        {
            error = "Task Git Worktree changed during checkpoint transaction";
            return false;
        }
        return true;
    }
}
