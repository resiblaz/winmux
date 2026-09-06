// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreCommandSqlite.h"

#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    namespace
    {
        bool _queryTaskBool(sqlite3* database, const char* sql, const TaskId& task, bool& value, std::string& error)
        {
            SqliteStatement statement{ database, sql, error };
            if (!statement || !BindText(database, statement.Get(), 1, task.Value(), error))
            {
                return false;
            }
            const auto result = sqlite3_step(statement.Get());
            if (result != SQLITE_ROW)
            {
                error = SqliteError(database, result, "query task boolean");
                return false;
            }
            value = sqlite3_column_int64(statement.Get(), 0) != 0;
            return true;
        }

        bool _nextBoundSequence(sqlite3* database, const char* sql, const std::string_view id, uint64_t& sequence, std::string& error)
        {
            SqliteStatement statement{ database, sql, error };
            if (!statement || !BindText(database, statement.Get(), 1, id, error))
            {
                return false;
            }
            const auto result = sqlite3_step(statement.Get());
            if (result != SQLITE_ROW)
            {
                error = SqliteError(database, result, "query next sequence");
                return false;
            }
            const auto next = sqlite3_column_int64(statement.Get(), 0);
            if (next <= 0)
            {
                error = "Sequence storage range is exhausted";
                return false;
            }
            sequence = static_cast<uint64_t>(next);
            return true;
        }
    }

    bool LoadTaskState(
        sqlite3* database,
        const TaskId& task,
        std::optional<StoredTaskState>& state,
        std::string& error)
    {
        SqliteStatement statement{ database, "SELECT version, status, manual_hold, created_at, updated_at FROM task WHERE id=?1;", error };
        if (!statement || !BindText(database, statement.Get(), 1, task.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step == SQLITE_DONE)
        {
            state.reset();
            return true;
        }
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "load task state");
            return false;
        }
        state = StoredTaskState{
            static_cast<uint64_t>(sqlite3_column_int64(statement.Get(), 0)),
            static_cast<TaskStatus>(sqlite3_column_int(statement.Get(), 1)),
            sqlite3_column_int(statement.Get(), 2) != 0,
            sqlite3_column_int64(statement.Get(), 3),
            sqlite3_column_int64(statement.Get(), 4),
        };
        return true;
    }

    bool IsTaskTombstoned(
        sqlite3* database,
        const TaskId& task,
        bool& tombstoned,
        std::string& error)
    {
        return _queryTaskBool(
            database,
            "SELECT EXISTS(SELECT 1 FROM delete_tombstone WHERE task_id=?1);",
            task,
            tombstoned,
            error);
    }

    TaskStoreCommandResult MissingTaskFailure(
        sqlite3* database,
        const TaskId& task,
        std::string& error)
    {
        bool tombstoned{};
        if (!IsTaskTombstoned(database, task, tombstoned, error))
        {
            return CommandFailure(TaskStoreCommandStatus::StorageError, error);
        }
        return tombstoned ?
                   CommandFailure(TaskStoreCommandStatus::Tombstoned, "Task was deleted and remains tombstoned") :
                   CommandFailure(TaskStoreCommandStatus::NotFound, "Task was not found");
    }

    bool LoadTaskRunState(
        sqlite3* database,
        const TaskRunId& run,
        std::optional<StoredTaskRunState>& state,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT task_id, state, created_at, started_at, finished_at, result, reason_code, agent_session_id
FROM task_run WHERE id=?1;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement || !BindText(database, statement.Get(), 1, run.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step == SQLITE_DONE)
        {
            state.reset();
            return true;
        }
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "load TaskRun state");
            return false;
        }
        state = StoredTaskRunState{
            TaskId{ ColumnText(statement.Get(), 0) },
            static_cast<TaskRunState>(sqlite3_column_int(statement.Get(), 1)),
            sqlite3_column_int64(statement.Get(), 2),
            ColumnOptionalInt64(statement.Get(), 3),
            ColumnOptionalInt64(statement.Get(), 4),
            ColumnText(statement.Get(), 5),
            ColumnText(statement.Get(), 6),
            std::nullopt,
        };
        if (const auto session = ColumnOptionalText(statement.Get(), 7))
        {
            state->AgentSession = AgentSessionId{ *session };
        }
        return true;
    }

    bool LoadTaskTerminalState(
        sqlite3* database,
        const TaskId& task,
        std::optional<StoredTaskTerminalState>& state,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT task_id, state, generation, updated_at
FROM task_terminal WHERE task_id=?1;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement || !BindText(database, statement.Get(), 1, task.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step == SQLITE_DONE)
        {
            state.reset();
            return true;
        }
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "load TaskTerminal state");
            return false;
        }
        state = StoredTaskTerminalState{
            TaskId{ ColumnText(statement.Get(), 0) },
            static_cast<TaskTerminalState>(sqlite3_column_int(statement.Get(), 1)),
            static_cast<uint64_t>(sqlite3_column_int64(statement.Get(), 2)),
            sqlite3_column_int64(statement.Get(), 3),
        };
        return true;
    }

    bool LoadTaskIntent(
        sqlite3* database,
        const TaskId& task,
        std::optional<TaskIntentRecord>& intent,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT id, task_id, kind, stage, revision, deadline_at, detail_json, reason_code,
       created_at, updated_at
FROM task_intent WHERE task_id=?1;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement || !BindText(database, statement.Get(), 1, task.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step == SQLITE_DONE)
        {
            intent.reset();
            return true;
        }
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "load Task intent");
            return false;
        }

        TaskIntentRecord loaded;
        loaded.Id = TaskIntentId{ ColumnText(statement.Get(), 0) };
        loaded.Task = TaskId{ ColumnText(statement.Get(), 1) };
        loaded.Kind = static_cast<TaskIntentKind>(sqlite3_column_int(statement.Get(), 2));
        loaded.Stage = static_cast<TaskIntentStage>(sqlite3_column_int(statement.Get(), 3));
        loaded.Revision = static_cast<uint64_t>(sqlite3_column_int64(statement.Get(), 4));
        loaded.DeadlineAtUnixMilliseconds = ColumnOptionalInt64(statement.Get(), 5);
        loaded.DetailJson = ColumnText(statement.Get(), 6);
        loaded.ReasonCode = ColumnText(statement.Get(), 7);
        loaded.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement.Get(), 8);
        loaded.UpdatedAtUnixMilliseconds = sqlite3_column_int64(statement.Get(), 9);
        intent = std::move(loaded);
        return true;
    }

    bool RowBelongsToTask(
        sqlite3* database,
        const char* sql,
        const TaskId& task,
        const std::string_view rowId,
        bool& value,
        std::string& error)
    {
        SqliteStatement statement{ database, sql, error };
        if (!statement ||
            !BindText(database, statement.Get(), 1, rowId, error) ||
            !BindText(database, statement.Get(), 2, task.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "validate aggregate reference");
            return false;
        }
        value = sqlite3_column_int(statement.Get(), 0) != 0;
        return true;
    }

    bool HasActiveRun(sqlite3* database, const TaskId& task, bool& value, std::string& error)
    {
        return _queryTaskBool(database, "SELECT EXISTS(SELECT 1 FROM task_run WHERE task_id=?1 AND state IN (0,1,2,3,4));", task, value, error);
    }

    bool HasUnclaimedMessages(sqlite3* database, const TaskId& task, bool& value, std::string& error)
    {
        return _queryTaskBool(database, "SELECT EXISTS(SELECT 1 FROM task_message WHERE task_id=?1 AND state IN (0,1));", task, value, error);
    }

    bool HasPendingAutomaticRetry(sqlite3* database, const TaskId& task, bool& value, std::string& error)
    {
        return _queryTaskBool(database, "SELECT EXISTS(SELECT 1 FROM task_run WHERE task_id=?1 AND trigger=2 AND state IN (0,1,2,3,4));", task, value, error);
    }

    bool NextAggregateSequence(sqlite3* database, const TaskId& task, uint64_t& sequence, std::string& error)
    {
        return _nextBoundSequence(database, "SELECT COALESCE(MAX(aggregate_sequence),0)+1 FROM task_event WHERE task_id=?1;", task.Value(), sequence, error);
    }

    bool NextRunSequence(sqlite3* database, const TaskId& task, uint64_t& sequence, std::string& error)
    {
        return _nextBoundSequence(database, "SELECT COALESCE(MAX(sequence),0)+1 FROM task_run WHERE task_id=?1;", task.Value(), sequence, error);
    }

    bool NextRunEventSequence(sqlite3* database, const TaskRunId& run, uint64_t& sequence, std::string& error)
    {
        return _nextBoundSequence(database, "SELECT COALESCE(MAX(run_sequence),0)+1 FROM task_event WHERE run_id=?1;", run.Value(), sequence, error);
    }

    bool UpdateTaskVersion(
        sqlite3* database,
        const TaskId& task,
        const uint64_t expectedVersion,
        const uint64_t committedVersion,
        const int64_t timestamp,
        std::string& error)
    {
        SqliteStatement statement{ database, "UPDATE task SET version=?1, updated_at=?2 WHERE id=?3 AND version=?4;", error };
        if (!statement ||
            !BindInt64(database, statement.Get(), 1, static_cast<int64_t>(committedVersion), error) ||
            !BindInt64(database, statement.Get(), 2, timestamp, error) ||
            !BindText(database, statement.Get(), 3, task.Value(), error) ||
            !BindInt64(database, statement.Get(), 4, static_cast<int64_t>(expectedVersion), error) ||
            !StepDone(database, statement.Get(), error))
        {
            return false;
        }
        if (sqlite3_changes(database) != 1)
        {
            error = "Task version changed during transaction";
            return false;
        }
        return true;
    }

    bool AppendEvent(
        sqlite3* database,
        const TaskCommandMetadata& metadata,
        const TaskId& task,
        const std::optional<TaskRunId>& run,
        const uint64_t aggregateSequence,
        const std::optional<uint64_t>& runSequence,
        const std::string_view type,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
INSERT INTO task_event(id, task_id, run_id, aggregate_sequence, run_sequence, type, payload_json, created_at)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);
)SQL";
        SqliteStatement statement{ database, sql, error };
        const auto runId = run ? std::optional<std::string>{ run->Value() } : std::nullopt;
        const auto runSequenceValue = runSequence ? std::optional<int64_t>{ static_cast<int64_t>(runSequence.value()) } : std::nullopt;
        return statement &&
               BindText(database, statement.Get(), 1, metadata.Event.Value(), error) &&
               BindText(database, statement.Get(), 2, task.Value(), error) &&
               BindOptionalText(database, statement.Get(), 3, runId, error) &&
               BindInt64(database, statement.Get(), 4, static_cast<int64_t>(aggregateSequence), error) &&
               BindOptionalInt64(database, statement.Get(), 5, runSequenceValue, error) &&
               BindText(database, statement.Get(), 6, type, error) &&
               BindText(database, statement.Get(), 7, metadata.EventPayloadJson, error) &&
               BindInt64(database, statement.Get(), 8, metadata.TimestampUnixMilliseconds, error) &&
               StepDone(database, statement.Get(), error);
    }

}
