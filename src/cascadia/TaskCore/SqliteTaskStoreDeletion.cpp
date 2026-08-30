// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreRowSqlite.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view DeleteTaskCommandName{ "DeleteTask" };

        bool _loadTombstone(
            sqlite3* database,
            const TaskId& task,
            std::optional<DeleteTombstoneRecord>& tombstone,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
SELECT task_id, deleted_at, expires_at, replay_digest, command_id, expected_version,
       committed_version, last_event_sequence
FROM delete_tombstone WHERE task_id=?1;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement || !details::BindText(database, statement.Get(), 1, task.Value(), error))
            {
                return false;
            }
            const auto step = sqlite3_step(statement.Get());
            if (step == SQLITE_DONE)
            {
                tombstone.reset();
                return true;
            }
            if (step != SQLITE_ROW)
            {
                error = details::SqliteError(database, step, "load Delete Task tombstone");
                return false;
            }
            tombstone = details::ReadDeleteTombstoneRow(statement.Get());
            return true;
        }

        std::optional<TaskStoreCommandResult> _evaluateTombstone(
            const std::optional<DeleteTombstoneRecord>& tombstone,
            const TaskCommandMetadata& metadata)
        {
            if (!tombstone)
            {
                return std::nullopt;
            }
            if (tombstone->Command && tombstone->Command.value() == metadata.Command)
            {
                if (tombstone->ReplayDigest != metadata.RequestDigest ||
                    tombstone->ExpectedTaskVersion != std::optional<uint64_t>{ metadata.ExpectedTaskVersion })
                {
                    return details::CommandFailure(
                        TaskStoreCommandStatus::IdempotencyConflict,
                        "Delete Task command ID was already used with a different request");
                }

                TaskStoreCommandResult result;
                result.Status = TaskStoreCommandStatus::Replayed;
                result.CommittedTaskVersion = tombstone->CommittedTaskVersion.value_or(0);
                result.LastEventSequence = tombstone->LastEventSequence.value_or(0);
                result.ResultJson = "{}";
                return result;
            }
            return details::CommandFailure(TaskStoreCommandStatus::Tombstoned, "Task was already deleted");
        }

        bool _insertTombstone(
            sqlite3* database,
            const DeleteTaskCommand& command,
            const uint64_t committedVersion,
            const uint64_t lastEventSequence,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
INSERT INTO delete_tombstone(task_id, deleted_at, expires_at, replay_digest, command_id,
                             expected_version, committed_version, last_event_sequence)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            return statement &&
                   details::BindText(database, statement.Get(), 1, command.Task.Value(), error) &&
                   details::BindInt64(database, statement.Get(), 2, command.Metadata.TimestampUnixMilliseconds, error) &&
                   details::BindInt64(
                       database,
                       statement.Get(),
                       3,
                       command.Metadata.TimestampUnixMilliseconds + TaskTombstoneRetentionMilliseconds,
                       error) &&
                   details::BindText(database, statement.Get(), 4, command.Metadata.RequestDigest, error) &&
                   details::BindText(database, statement.Get(), 5, command.Metadata.Command.Value(), error) &&
                   details::BindInt64(database, statement.Get(), 6, static_cast<int64_t>(command.Metadata.ExpectedTaskVersion), error) &&
                   details::BindInt64(database, statement.Get(), 7, static_cast<int64_t>(committedVersion), error) &&
                   details::BindInt64(database, statement.Get(), 8, static_cast<int64_t>(lastEventSequence), error) &&
                   details::StepDone(database, statement.Get(), error);
        }

        bool _deleteAggregate(sqlite3* database, const TaskId& task, std::string& error)
        {
            details::SqliteStatement statement{ database, "DELETE FROM task WHERE id=?1;", error };
            if (!statement ||
                !details::BindText(database, statement.Get(), 1, task.Value(), error) ||
                !details::StepDone(database, statement.Get(), error))
            {
                return false;
            }
            if (sqlite3_changes(database) != 1)
            {
                error = "Task aggregate changed during deletion";
                return false;
            }
            return true;
        }
    }

    TaskStoreCommandResult SqliteTaskStore::DeleteTask(const DeleteTaskCommand& command)
    {
        if (!IsReady())
        {
            return details::CommandFailure(TaskStoreCommandStatus::NotReady, LastError());
        }

        std::string error;
        if (!details::ValidateCommandMetadata(command.Metadata, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, std::move(error));
        }

        std::scoped_lock lock{ _impl->DatabaseMutex };
        details::SqliteTransaction transaction{ _impl->Database, error };
        if (!transaction)
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }

        std::optional<details::StoredReceipt> receipt;
        if (!details::LookupReceipt(_impl->Database, command.Metadata.Command, receipt, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Task, DeleteTaskCommandName))
        {
            return replay.value();
        }

        std::optional<DeleteTombstoneRecord> tombstone;
        if (!_loadTombstone(_impl->Database, command.Task, tombstone, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (const auto replay = _evaluateTombstone(tombstone, command.Metadata))
        {
            return replay.value();
        }

        if (!details::ValidateDeleteTaskCommand(command, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, std::move(error));
        }

        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(_impl->Database, command.Task, task, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!task)
        {
            return details::CommandFailure(TaskStoreCommandStatus::NotFound, "Task was not found");
        }
        if (task->Version != command.Metadata.ExpectedTaskVersion)
        {
            return details::CommandFailure(TaskStoreCommandStatus::VersionConflict, "Task version does not match expected version");
        }
        if (command.Metadata.TimestampUnixMilliseconds < task->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Command timestamp precedes the Task update time");
        }
        if (task->Version == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, "Task version storage range is exhausted");
        }

        std::optional<TaskIntentRecord> intent;
        if (!details::LoadTaskIntent(_impl->Database, command.Task, intent, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!intent || intent->Id != command.Intent || intent->Kind != TaskIntentKind::Delete)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "Matching Delete Task intent was not found");
        }
        if (intent->Revision != command.ExpectedIntentRevision)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::VersionConflict,
                "Delete Task intent revision does not match expected revision",
                TransitionError::IntentRevisionMismatch);
        }
        if (intent->Stage != TaskIntentStage::DeletingAggregate)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "Delete Task intent has not reached aggregate deletion");
        }

        bool hasActiveRun{};
        if (!details::HasActiveRun(_impl->Database, command.Task, hasActiveRun, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (hasActiveRun)
        {
            return details::CommandFailure(TaskStoreCommandStatus::ActiveRunExists, "Delete Task requires no active TaskRun");
        }

        uint64_t eventSequence{};
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Task, eventSequence, error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Task,
                std::nullopt,
                eventSequence,
                std::nullopt,
                "TaskDeleted",
                error) ||
            !_insertTombstone(_impl->Database, command, committedVersion, eventSequence, error) ||
            !_deleteAggregate(_impl->Database, command.Task, error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }

        TaskStoreCommandResult result;
        result.Status = TaskStoreCommandStatus::Succeeded;
        result.CommittedTaskVersion = committedVersion;
        result.LastEventSequence = eventSequence;
        result.ResultJson = "{}";
        return result;
    }

    std::optional<DeleteTombstoneRecord> SqliteTaskStore::LoadDeleteTombstone(const TaskId& task) const
    {
        if (!IsReady() || !IsCanonicalUuid(task.Value()))
        {
            return std::nullopt;
        }
        std::scoped_lock lock{ _impl->DatabaseMutex };
        std::string error;
        std::optional<DeleteTombstoneRecord> tombstone;
        return _loadTombstone(_impl->Database, task, tombstone, error) ? tombstone : std::nullopt;
    }

    TaskStoreMaintenanceResult SqliteTaskStore::PurgeExpiredTombstones(
        const int64_t nowUnixMilliseconds,
        const size_t limit)
    {
        TaskStoreMaintenanceResult result;
        if (!IsReady())
        {
            result.Status = TaskStoreMaintenanceStatus::NotReady;
            result.Error = LastError();
            return result;
        }
        if (nowUnixMilliseconds < 0 || limit == 0)
        {
            result.Status = TaskStoreMaintenanceStatus::InvalidRequest;
            result.Error = "Tombstone cleanup request is invalid";
            return result;
        }

        std::scoped_lock lock{ _impl->DatabaseMutex };
        std::string error;
        details::SqliteTransaction transaction{ _impl->Database, error };
        if (!transaction)
        {
            result.Error = std::move(error);
            return result;
        }

        constexpr auto deleteSql = R"SQL(
DELETE FROM delete_tombstone
WHERE task_id IN (
    SELECT task_id FROM delete_tombstone
    WHERE expires_at<=?1
    ORDER BY expires_at, task_id
    LIMIT ?2
);
)SQL";
        const auto boundedLimit = std::min(limit, MaximumTombstoneCleanupBatch);
        details::SqliteStatement statement{ _impl->Database, deleteSql, error };
        if (!statement ||
            !details::BindInt64(_impl->Database, statement.Get(), 1, nowUnixMilliseconds, error) ||
            !details::BindInt64(_impl->Database, statement.Get(), 2, static_cast<int64_t>(boundedLimit), error) ||
            !details::StepDone(_impl->Database, statement.Get(), error))
        {
            result.Error = std::move(error);
            return result;
        }
        result.RemovedCount = static_cast<size_t>(sqlite3_changes(_impl->Database));

        {
            details::SqliteStatement remaining{
                _impl->Database,
                "SELECT EXISTS(SELECT 1 FROM delete_tombstone WHERE expires_at<=?1);",
                error
            };
            if (!remaining ||
                !details::BindInt64(_impl->Database, remaining.Get(), 1, nowUnixMilliseconds, error) ||
                sqlite3_step(remaining.Get()) != SQLITE_ROW)
            {
                result.Error = error.empty() ? "Failed to query remaining tombstone maintenance work" : std::move(error);
                return result;
            }
            result.HasMore = sqlite3_column_int(remaining.Get(), 0) != 0;
        }
        if (!transaction.Commit(error))
        {
            result.Error = std::move(error);
            return result;
        }
        result.Status = TaskStoreMaintenanceStatus::Succeeded;
        return result;
    }
}
