// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreMessageSqlite.h"

#include "TaskStoreRowSqlite.h"
#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    bool LoadActiveTaskRun(
        sqlite3* database,
        const TaskId& task,
        std::optional<StoredActiveTaskRun>& run,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT id, state FROM task_run
WHERE task_id=?1 AND state IN (0,1,2,3,4)
LIMIT 1;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement || !BindText(database, statement.Get(), 1, task.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step == SQLITE_DONE)
        {
            run.reset();
            return true;
        }
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "load active TaskRun");
            return false;
        }
        run = StoredActiveTaskRun{
            TaskRunId{ ColumnText(statement.Get(), 0) },
            static_cast<TaskRunState>(sqlite3_column_int(statement.Get(), 1)),
        };
        return true;
    }

    bool LoadTaskMessage(
        sqlite3* database,
        const TaskMessageId& message,
        std::optional<TaskMessageRecord>& record,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT id, task_id, author, state, revision, content, claimed_by_run_id, created_at, updated_at
FROM task_message WHERE id=?1;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement || !BindText(database, statement.Get(), 1, message.Value(), error))
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
            error = SqliteError(database, step, "load Task message");
            return false;
        }
        record = ReadTaskMessageRow(statement.Get());
        return true;
    }

    bool CountUnclaimedTaskMessages(
        sqlite3* database,
        const TaskId& task,
        uint64_t& count,
        std::string& error)
    {
        SqliteStatement statement{
            database,
            "SELECT count(*) FROM task_message WHERE task_id=?1 AND state IN (0,1);",
            error
        };
        if (!statement || !BindText(database, statement.Get(), 1, task.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "count unclaimed Task messages");
            return false;
        }
        count = static_cast<uint64_t>(sqlite3_column_int64(statement.Get(), 0));
        return true;
    }

    bool InsertTaskMessage(
        sqlite3* database,
        const TaskMessageRecord& message,
        const TaskMessageState state,
        const std::optional<TaskRunId>& claimedByRun,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
INSERT INTO task_message(id, task_id, state, revision, content, claimed_by_run_id,
                         created_at, author, updated_at)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);
)SQL";
        SqliteStatement statement{ database, sql, error };
        const auto runId = claimedByRun ?
                               std::optional<std::string>{ claimedByRun->Value() } :
                               std::nullopt;
        return statement &&
               BindText(database, statement.Get(), 1, message.Id.Value(), error) &&
               BindText(database, statement.Get(), 2, message.Task.Value(), error) &&
               BindInt64(database, statement.Get(), 3, static_cast<int64_t>(state), error) &&
               BindInt64(database, statement.Get(), 4, static_cast<int64_t>(message.Revision), error) &&
               BindText(database, statement.Get(), 5, message.Content, error) &&
               BindOptionalText(database, statement.Get(), 6, runId, error) &&
               BindInt64(database, statement.Get(), 7, message.CreatedAtUnixMilliseconds, error) &&
               BindInt64(database, statement.Get(), 8, static_cast<int64_t>(message.Author), error) &&
               BindInt64(database, statement.Get(), 9, message.UpdatedAtUnixMilliseconds, error) &&
               StepDone(database, statement.Get(), error);
    }

    bool NextRunMessagePosition(
        sqlite3* database,
        const TaskRunId& run,
        uint64_t& position,
        std::string& error)
    {
        SqliteStatement statement{
            database,
            "SELECT COALESCE(MAX(position),-1)+1 FROM task_run_message WHERE run_id=?1;",
            error
        };
        if (!statement || !BindText(database, statement.Get(), 1, run.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "query next run message position");
            return false;
        }
        const auto next = sqlite3_column_int64(statement.Get(), 0);
        if (next < 0)
        {
            error = "Run message position storage range is exhausted";
            return false;
        }
        position = static_cast<uint64_t>(next);
        return true;
    }

    bool InsertTaskRunMessage(
        sqlite3* database,
        const TaskRunId& run,
        const TaskMessageRecord& message,
        const uint64_t position,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
INSERT INTO task_run_message(run_id, message_id, message_revision, canonical_content, position)
VALUES(?1, ?2, ?3, ?4, ?5);
)SQL";
        SqliteStatement statement{ database, sql, error };
        return statement &&
               BindText(database, statement.Get(), 1, run.Value(), error) &&
               BindText(database, statement.Get(), 2, message.Id.Value(), error) &&
               BindInt64(database, statement.Get(), 3, static_cast<int64_t>(message.Revision), error) &&
               BindText(database, statement.Get(), 4, message.Content, error) &&
               BindInt64(database, statement.Get(), 5, static_cast<int64_t>(position), error) &&
               StepDone(database, statement.Get(), error);
    }

    bool LoadClaimableTaskMessages(
        sqlite3* database,
        const TaskId& task,
        const size_t maximumCount,
        std::vector<TaskMessageRecord>& messages,
        bool& exceedsMaximum,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT id, task_id, author, state, revision, content, claimed_by_run_id, created_at, updated_at
FROM task_message
WHERE task_id=?1 AND state IN (0,1) AND claimed_by_run_id IS NULL
ORDER BY created_at, id
LIMIT ?2;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement ||
            !BindText(database, statement.Get(), 1, task.Value(), error) ||
            !BindInt64(database, statement.Get(), 2, static_cast<int64_t>(maximumCount + 1), error))
        {
            return false;
        }
        for (auto step = sqlite3_step(statement.Get()); step != SQLITE_DONE; step = sqlite3_step(statement.Get()))
        {
            if (step != SQLITE_ROW)
            {
                error = SqliteError(database, step, "load claimable Task messages");
                return false;
            }
            messages.emplace_back(ReadTaskMessageRow(statement.Get()));
        }
        exceedsMaximum = messages.size() > maximumCount;
        return true;
    }

    bool ClaimTaskMessage(
        sqlite3* database,
        const TaskMessageRecord& message,
        const TaskRunId& run,
        const int64_t timestamp,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
UPDATE task_message
SET state=2, claimed_by_run_id=?1, updated_at=?2
WHERE id=?3 AND task_id=?4 AND revision=?5 AND state IN (0,1) AND claimed_by_run_id IS NULL;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement ||
            !BindText(database, statement.Get(), 1, run.Value(), error) ||
            !BindInt64(database, statement.Get(), 2, timestamp, error) ||
            !BindText(database, statement.Get(), 3, message.Id.Value(), error) ||
            !BindText(database, statement.Get(), 4, message.Task.Value(), error) ||
            !BindInt64(database, statement.Get(), 5, static_cast<int64_t>(message.Revision), error) ||
            !StepDone(database, statement.Get(), error))
        {
            return false;
        }
        if (sqlite3_changes(database) != 1)
        {
            error = "Task message changed during claim transaction";
            return false;
        }
        return true;
    }
}
