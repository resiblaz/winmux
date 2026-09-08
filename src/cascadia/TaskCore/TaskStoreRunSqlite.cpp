// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreRunSqlite.h"

#include "TaskStoreCommandSqlite.h"
#include "TaskStoreGitCheckpointSqlite.h"
#include "TaskStoreGitSqlite.h"
#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    namespace
    {
        std::optional<bool> _referenceBelongsToTask(
            sqlite3* database,
            const char* sql,
            const TaskId& task,
            const std::string_view id,
            std::string& error)
        {
            bool belongs{};
            if (!RowBelongsToTask(database, sql, task, id, belongs, error))
            {
                return std::nullopt;
            }
            return belongs;
        }

        bool _freezeExecutionContextIdentity(
            sqlite3* database,
            const TaskRunRecord& run,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE execution_context SET identity_frozen=1
WHERE id=?1 AND task_id=?2 AND identity_frozen=0;
)SQL";
            SqliteStatement statement{ database, sql, error };
            if (!statement ||
                !BindText(database, statement.Get(), 1, run.ExecutionContext.Value(), error) ||
                !BindText(database, statement.Get(), 2, run.Task.Value(), error) ||
                !StepDone(database, statement.Get(), error))
            {
                return false;
            }
            if (sqlite3_changes(database) == 1)
            {
                return true;
            }

            const auto frozen = _referenceBelongsToTask(
                database,
                "SELECT EXISTS(SELECT 1 FROM execution_context WHERE id=?1 AND task_id=?2 AND identity_frozen=1);",
                run.Task,
                run.ExecutionContext.Value(),
                error);
            if (!frozen)
            {
                return false;
            }
            if (!frozen.value())
            {
                error = "TaskRun ExecutionContext identity could not be frozen";
                return false;
            }
            return true;
        }
    }

    TaskRunReferenceValidation ValidateTaskRunReferences(
        sqlite3* database,
        const TaskRunRecord& run,
        std::string& error)
    {
        SqliteStatement project{ database, R"SQL(
SELECT EXISTS(SELECT 1 FROM task t JOIN project p ON p.workspace_id=t.workspace_id WHERE t.id=?1 AND p.archived_at IS NOT NULL)
 OR EXISTS(SELECT 1 FROM task_board_operation WHERE task_id=?1 AND stage=0);
)SQL", error };
        if (!project || !BindText(database, project.Get(), 1, run.Task.Value(), error) || sqlite3_step(project.Get()) != SQLITE_ROW)
            return TaskRunReferenceValidation::StorageError;
        if (sqlite3_column_int(project.Get(), 0)) { error = "project_archived_or_board_operation_pending"; return TaskRunReferenceValidation::Invalid; }
        bool incompleteCheckpoint{};
        if (!TaskHasIncompleteGitCheckpoint(
                database,
                run.Task,
                incompleteCheckpoint,
                error))
        {
            return TaskRunReferenceValidation::StorageError;
        }
        if (incompleteCheckpoint)
        {
            error = "A previous TaskRun Git checkpoint requires recovery";
            return TaskRunReferenceValidation::Invalid;
        }
        bool unavailableWorktree{};
        if (!TaskGitWorktreeBlocksRun(
                database,
                run.Task,
                unavailableWorktree,
                error))
        {
            return TaskRunReferenceValidation::StorageError;
        }
        if (unavailableWorktree)
        {
            error = "The managed Task Worktree is not ready for another TaskRun";
            return TaskRunReferenceValidation::Invalid;
        }
        const auto executionContext = _referenceBelongsToTask(
            database,
            "SELECT EXISTS(SELECT 1 FROM execution_context WHERE id=?1 AND task_id=?2);",
            run.Task,
            run.ExecutionContext.Value(),
            error);
        if (!executionContext)
        {
            return TaskRunReferenceValidation::StorageError;
        }
        if (!executionContext.value())
        {
            error = "ExecutionContext does not belong to the Task";
            return TaskRunReferenceValidation::Invalid;
        }
        if (run.AgentSession)
        {
            const auto agentSession = _referenceBelongsToTask(
                database,
                "SELECT EXISTS(SELECT 1 FROM agent_session WHERE id=?1 AND task_id=?2);",
                run.Task,
                run.AgentSession->Value(),
                error);
            if (!agentSession)
            {
                return TaskRunReferenceValidation::StorageError;
            }
            if (!agentSession.value())
            {
                error = "AgentSession does not belong to the Task";
                return TaskRunReferenceValidation::Invalid;
            }
        }
        if (run.RetryOfRunId)
        {
            const auto retryRun = _referenceBelongsToTask(
                database,
                "SELECT EXISTS(SELECT 1 FROM task_run WHERE id=?1 AND task_id=?2 AND state IN (5,6,7,8));",
                run.Task,
                run.RetryOfRunId->Value(),
                error);
            if (!retryRun)
            {
                return TaskRunReferenceValidation::StorageError;
            }
            if (!retryRun.value())
            {
                error = "Retry source TaskRun does not belong to the Task";
                return TaskRunReferenceValidation::Invalid;
            }
        }
        return TaskRunReferenceValidation::Valid;
    }

    bool InsertTaskRun(
        sqlite3* database,
        const TaskRunRecord& run,
        const uint64_t sequence,
        std::string& error)
    {
        if (!_freezeExecutionContextIdentity(database, run, error))
        {
            return false;
        }

        constexpr auto sql = R"SQL(
INSERT INTO task_run(id, task_id, execution_context_id, agent_session_id, retry_of_run_id,
                     trigger, state, sequence, created_at, started_at, finished_at, result, reason_code)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, NULL, NULL, ?10, ?11);
)SQL";
        SqliteStatement statement{ database, sql, error };
        const auto agentSession = run.AgentSession ?
                                      std::optional<std::string>{ run.AgentSession->Value() } :
                                      std::nullopt;
        const auto retryOf = run.RetryOfRunId ?
                                 std::optional<std::string>{ run.RetryOfRunId->Value() } :
                                 std::nullopt;
        const auto inserted = statement &&
               BindText(database, statement.Get(), 1, run.Id.Value(), error) &&
               BindText(database, statement.Get(), 2, run.Task.Value(), error) &&
               BindText(database, statement.Get(), 3, run.ExecutionContext.Value(), error) &&
               BindOptionalText(database, statement.Get(), 4, agentSession, error) &&
               BindOptionalText(database, statement.Get(), 5, retryOf, error) &&
               BindInt64(database, statement.Get(), 6, static_cast<int64_t>(run.Trigger), error) &&
               BindInt64(database, statement.Get(), 7, static_cast<int64_t>(run.State), error) &&
               BindInt64(database, statement.Get(), 8, static_cast<int64_t>(sequence), error) &&
               BindInt64(database, statement.Get(), 9, run.CreatedAtUnixMilliseconds, error) &&
               BindText(database, statement.Get(), 10, run.Result, error) &&
               BindText(database, statement.Get(), 11, run.ReasonCode, error) &&
               StepDone(database, statement.Get(), error);
        if (!inserted) return false;
        // A modern project Run is an execution request, including queued follow-up
        // messages. Keep its planning status in the same transaction as the Run.
        // Legacy execution contexts retain the pre-project command semantics.
        SqliteStatement progress{ database, R"SQL(
UPDATE task SET status=2 WHERE id=?1 AND status IN(1,3,4)
 AND EXISTS(SELECT 1 FROM execution_context WHERE id=?2 AND task_id=?1 AND project_version>0);
)SQL", error };
        return progress && BindText(database, progress.Get(), 1, run.Task.Value(), error) &&
               BindText(database, progress.Get(), 2, run.ExecutionContext.Value(), error) && StepDone(database, progress.Get(), error);
    }
}
