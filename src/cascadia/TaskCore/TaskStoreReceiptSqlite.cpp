// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreCommandSqlite.h"

#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    TaskStoreCommandResult CommandFailure(
        const TaskStoreCommandStatus status,
        std::string error,
        const TransitionError transition)
    {
        TaskStoreCommandResult result;
        result.Status = status;
        result.Transition = transition;
        result.Error = std::move(error);
        return result;
    }

    bool LookupReceipt(
        sqlite3* database,
        const CommandId& command,
        std::optional<StoredReceipt>& receipt,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
SELECT task_id, command_name, request_digest, expected_version,
       committed_version, last_event_sequence, result_json
FROM command_receipt WHERE id=?1;
)SQL";
        SqliteStatement statement{ database, sql, error };
        if (!statement || !BindText(database, statement.Get(), 1, command.Value(), error))
        {
            return false;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step == SQLITE_DONE)
        {
            constexpr auto tombstoneSql = R"SQL(
SELECT task_id, replay_digest, expected_version, committed_version, last_event_sequence
FROM delete_tombstone WHERE command_id=?1;
)SQL";
            SqliteStatement tombstone{ database, tombstoneSql, error };
            if (!tombstone || !BindText(database, tombstone.Get(), 1, command.Value(), error))
            {
                return false;
            }
            const auto tombstoneStep = sqlite3_step(tombstone.Get());
            if (tombstoneStep == SQLITE_DONE)
            {
                receipt.reset();
                return true;
            }
            if (tombstoneStep != SQLITE_ROW ||
                sqlite3_column_type(tombstone.Get(), 2) == SQLITE_NULL ||
                sqlite3_column_type(tombstone.Get(), 3) == SQLITE_NULL ||
                sqlite3_column_type(tombstone.Get(), 4) == SQLITE_NULL)
            {
                error = tombstoneStep == SQLITE_ROW ?
                            "Delete tombstone command receipt is incomplete" :
                            SqliteError(database, tombstoneStep, "lookup tombstone command receipt");
                return false;
            }

            StoredReceipt loaded;
            loaded.Task = TaskId{ ColumnText(tombstone.Get(), 0) };
            loaded.CommandName = "DeleteTask";
            loaded.RequestDigest = ColumnText(tombstone.Get(), 1);
            loaded.ExpectedVersion = static_cast<uint64_t>(sqlite3_column_int64(tombstone.Get(), 2));
            loaded.CommittedVersion = static_cast<uint64_t>(sqlite3_column_int64(tombstone.Get(), 3));
            loaded.LastEventSequence = static_cast<uint64_t>(sqlite3_column_int64(tombstone.Get(), 4));
            loaded.ResultJson = "{}";
            receipt = std::move(loaded);
            return true;
        }
        if (step != SQLITE_ROW)
        {
            error = SqliteError(database, step, "lookup command receipt");
            return false;
        }

        StoredReceipt loaded;
        loaded.Task = TaskId{ ColumnText(statement.Get(), 0) };
        loaded.CommandName = ColumnText(statement.Get(), 1);
        loaded.RequestDigest = ColumnText(statement.Get(), 2);
        loaded.ExpectedVersion = static_cast<uint64_t>(sqlite3_column_int64(statement.Get(), 3));
        loaded.CommittedVersion = static_cast<uint64_t>(sqlite3_column_int64(statement.Get(), 4));
        loaded.LastEventSequence = static_cast<uint64_t>(sqlite3_column_int64(statement.Get(), 5));
        loaded.ResultJson = ColumnText(statement.Get(), 6);
        receipt = std::move(loaded);
        return true;
    }

    std::optional<TaskStoreCommandResult> EvaluateReceiptReplay(
        const std::optional<StoredReceipt>& receipt,
        const TaskCommandMetadata& metadata,
        const TaskId& task,
        const std::string_view commandName)
    {
        if (!receipt)
        {
            return std::nullopt;
        }
        if (receipt->Task != task || receipt->CommandName != commandName ||
            receipt->RequestDigest != metadata.RequestDigest || receipt->ExpectedVersion != metadata.ExpectedTaskVersion)
        {
            return CommandFailure(TaskStoreCommandStatus::IdempotencyConflict, "Command ID was already used for a different request");
        }

        TaskStoreCommandResult result;
        result.Status = TaskStoreCommandStatus::Replayed;
        result.CommittedTaskVersion = receipt->CommittedVersion;
        result.LastEventSequence = receipt->LastEventSequence;
        result.ResultJson = receipt->ResultJson;
        return result;
    }

    bool InsertReceipt(
        sqlite3* database,
        const TaskCommandMetadata& metadata,
        const TaskId& task,
        const std::string_view commandName,
        const uint64_t committedVersion,
        const uint64_t lastEventSequence,
        std::string& error)
    {
        constexpr auto sql = R"SQL(
INSERT INTO command_receipt(id, task_id, command_name, request_digest, expected_version,
                            committed_version, last_event_sequence, result_json, created_at)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);
)SQL";
        SqliteStatement statement{ database, sql, error };
        return statement &&
               BindText(database, statement.Get(), 1, metadata.Command.Value(), error) &&
               BindText(database, statement.Get(), 2, task.Value(), error) &&
               BindText(database, statement.Get(), 3, commandName, error) &&
               BindText(database, statement.Get(), 4, metadata.RequestDigest, error) &&
               BindInt64(database, statement.Get(), 5, static_cast<int64_t>(metadata.ExpectedTaskVersion), error) &&
               BindInt64(database, statement.Get(), 6, static_cast<int64_t>(committedVersion), error) &&
               BindInt64(database, statement.Get(), 7, static_cast<int64_t>(lastEventSequence), error) &&
               BindText(database, statement.Get(), 8, metadata.ReceiptResultJson, error) &&
               BindInt64(database, statement.Get(), 9, metadata.TimestampUnixMilliseconds, error) &&
               StepDone(database, statement.Get(), error);
    }
}
