// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskStoreBoardSqlite.h"
#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    bool ReadBoardReceipt(sqlite3* db, const TaskBoardCommand& command, std::optional<TaskBoardCommandResult>& result, std::string& error)
    {
        SqliteStatement row{ db, "SELECT payload_hash,task_id,task_version,column_revision,event_sequence,run_id,operation_id,pending FROM board_command_receipt WHERE command_id=?1;", error };
        if (!row || !BindText(db, row.Get(), 1, command.Metadata.Command.Value(), error)) return false;
        const auto step = sqlite3_step(row.Get());
        if (step == SQLITE_DONE) { result.reset(); return true; }
        if (step != SQLITE_ROW) { error = SqliteError(db, step, "read board receipt"); return false; }
        TaskBoardCommandResult value;
        if (ColumnText(row.Get(), 0) != command.Metadata.RequestDigest || ColumnText(row.Get(), 1) != command.Task.Value())
        { value.Status = TaskStoreCommandStatus::IdempotencyConflict; value.Error = "board_command_reused"; result = std::move(value); return true; }
        value.Status = TaskStoreCommandStatus::Replayed; value.Task = command.Task;
        value.TaskVersion = sqlite3_column_int64(row.Get(), 2); value.ColumnRevision = sqlite3_column_int64(row.Get(), 3);
        value.EventSequence = sqlite3_column_int64(row.Get(), 4);
        if (const auto id = ColumnOptionalText(row.Get(), 5)) value.Run = TaskRunId{ *id };
        if (const auto id = ColumnOptionalText(row.Get(), 6)) value.Operation = CommandId{ *id };
        value.Pending = sqlite3_column_int(row.Get(), 7) != 0;
        result = std::move(value);
        return true;
    }

    bool WriteBoardReceipt(sqlite3* db, const TaskBoardCommand& command, const TaskBoardCommandResult& result, std::string& error)
    {
        SqliteStatement row{ db, "INSERT INTO board_command_receipt(command_id,payload_hash,task_id,task_version,column_revision,event_sequence,run_id,operation_id,pending) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);", error };
        return row && BindText(db, row.Get(), 1, command.Metadata.Command.Value(), error) &&
               BindText(db, row.Get(), 2, command.Metadata.RequestDigest, error) && BindText(db, row.Get(), 3, command.Task.Value(), error) &&
               BindInt64(db, row.Get(), 4, static_cast<int64_t>(result.TaskVersion), error) &&
               BindInt64(db, row.Get(), 5, static_cast<int64_t>(result.ColumnRevision), error) &&
               BindInt64(db, row.Get(), 6, static_cast<int64_t>(result.EventSequence), error) &&
               BindOptionalText(db, row.Get(), 7, result.Run ? std::optional{ result.Run->Value() } : std::nullopt, error) &&
               BindOptionalText(db, row.Get(), 8, result.Operation ? std::optional{ result.Operation->Value() } : std::nullopt, error) &&
               BindInt64(db, row.Get(), 9, result.Pending, error) && StepDone(db, row.Get(), error);
    }
}
