// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskIntentStatePolicy.h"
#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view StartTaskIntentCommandName{ "StartTaskIntent" };
        constexpr std::string_view AdvanceTaskIntentCommandName{ "AdvanceTaskIntent" };

        bool _writeNewIntent(
            sqlite3* database,
            const TaskIntentRecord& intent,
            const bool replaceCompleted,
            std::string& error)
        {
            constexpr auto insertSql = R"SQL(
INSERT INTO task_intent(id, task_id, kind, stage, revision, deadline_at, detail_json,
                        reason_code, created_at, updated_at)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);
)SQL";
            constexpr auto replaceSql = R"SQL(
UPDATE task_intent
SET id=?1, kind=?3, stage=?4, revision=?5, deadline_at=?6, detail_json=?7,
    reason_code=?8, created_at=?9, updated_at=?10
WHERE task_id=?2 AND stage=8;
)SQL";
            details::SqliteStatement statement{ database, replaceCompleted ? replaceSql : insertSql, error };
            const auto deadline = intent.DeadlineAtUnixMilliseconds;
            if (!statement ||
                !details::BindText(database, statement.Get(), 1, intent.Id.Value(), error) ||
                !details::BindText(database, statement.Get(), 2, intent.Task.Value(), error) ||
                !details::BindInt64(database, statement.Get(), 3, static_cast<int64_t>(intent.Kind), error) ||
                !details::BindInt64(database, statement.Get(), 4, static_cast<int64_t>(intent.Stage), error) ||
                !details::BindInt64(database, statement.Get(), 5, static_cast<int64_t>(intent.Revision), error) ||
                !details::BindOptionalInt64(database, statement.Get(), 6, deadline, error) ||
                !details::BindText(database, statement.Get(), 7, intent.DetailJson, error) ||
                !details::BindText(database, statement.Get(), 8, intent.ReasonCode, error) ||
                !details::BindInt64(database, statement.Get(), 9, intent.CreatedAtUnixMilliseconds, error) ||
                !details::BindInt64(database, statement.Get(), 10, intent.UpdatedAtUnixMilliseconds, error) ||
                !details::StepDone(database, statement.Get(), error))
            {
                return false;
            }
            if (replaceCompleted && sqlite3_changes(database) != 1)
            {
                error = "Completed Task intent changed during transaction";
                return false;
            }
            return true;
        }

        bool _advanceIntent(
            sqlite3* database,
            const AdvanceTaskIntentCommand& command,
            const TaskIntentRecord& current,
            const uint64_t committedRevision,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE task_intent
SET stage=?1, revision=?2, deadline_at=?3, detail_json=?4, reason_code=?5, updated_at=?6
WHERE id=?7 AND task_id=?8 AND revision=?9 AND stage=?10;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement ||
                !details::BindInt64(database, statement.Get(), 1, static_cast<int64_t>(command.RequestedStage), error) ||
                !details::BindInt64(database, statement.Get(), 2, static_cast<int64_t>(committedRevision), error) ||
                !details::BindOptionalInt64(database, statement.Get(), 3, command.DeadlineAtUnixMilliseconds, error) ||
                !details::BindText(database, statement.Get(), 4, command.DetailJson, error) ||
                !details::BindText(database, statement.Get(), 5, command.ReasonCode, error) ||
                !details::BindInt64(database, statement.Get(), 6, command.Metadata.TimestampUnixMilliseconds, error) ||
                !details::BindText(database, statement.Get(), 7, command.Intent.Value(), error) ||
                !details::BindText(database, statement.Get(), 8, command.Task.Value(), error) ||
                !details::BindInt64(database, statement.Get(), 9, static_cast<int64_t>(command.ExpectedIntentRevision), error) ||
                !details::BindInt64(database, statement.Get(), 10, static_cast<int64_t>(current.Stage), error) ||
                !details::StepDone(database, statement.Get(), error))
            {
                return false;
            }
            if (sqlite3_changes(database) != 1)
            {
                error = "Task intent changed during transaction";
                return false;
            }
            return true;
        }

        bool _updateTaskForIntent(
            sqlite3* database,
            const TaskId& task,
            const uint64_t expectedVersion,
            const uint64_t committedVersion,
            const int64_t timestamp,
            const bool setManualHold,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE task
SET manual_hold=CASE WHEN ?1=1 THEN 1 ELSE manual_hold END,
    version=?2,
    updated_at=?3
WHERE id=?4 AND version=?5;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement ||
                !details::BindInt64(database, statement.Get(), 1, setManualHold ? 1 : 0, error) ||
                !details::BindInt64(database, statement.Get(), 2, static_cast<int64_t>(committedVersion), error) ||
                !details::BindInt64(database, statement.Get(), 3, timestamp, error) ||
                !details::BindText(database, statement.Get(), 4, task.Value(), error) ||
                !details::BindInt64(database, statement.Get(), 5, static_cast<int64_t>(expectedVersion), error) ||
                !details::StepDone(database, statement.Get(), error))
            {
                return false;
            }
            if (sqlite3_changes(database) != 1)
            {
                error = "Task version changed during intent transaction";
                return false;
            }
            return true;
        }

        TaskStoreCommandResult _success(
            const TaskCommandMetadata& metadata,
            const uint64_t committedVersion,
            const uint64_t eventSequence)
        {
            TaskStoreCommandResult result;
            result.Status = TaskStoreCommandStatus::Succeeded;
            result.CommittedTaskVersion = committedVersion;
            result.LastEventSequence = eventSequence;
            result.ResultJson = metadata.ReceiptResultJson;
            return result;
        }

        std::optional<TaskStoreCommandResult> _validateTaskMutation(
            sqlite3* database,
            const TaskId& taskId,
            const std::optional<details::StoredTaskState>& task,
            const TaskCommandMetadata& metadata,
            std::string& error)
        {
            if (!task)
            {
                return details::MissingTaskFailure(database, taskId, error);
            }
            if (task->Version != metadata.ExpectedTaskVersion)
            {
                return details::CommandFailure(TaskStoreCommandStatus::VersionConflict, "Task version does not match expected version");
            }
            if (metadata.TimestampUnixMilliseconds < task->UpdatedAtUnixMilliseconds)
            {
                return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Command timestamp precedes the Task update time");
            }
            if (task->Version == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, "Task version storage range is exhausted");
            }
            return std::nullopt;
        }
    }

    TaskStoreCommandResult SqliteTaskStore::StartTaskIntent(const StartTaskIntentCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Intent.Task, StartTaskIntentCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateStartTaskIntentCommand(command, error) ||
            !IsTaskIntentStageSupported(command.Intent.Kind, command.Intent.Stage))
        {
            if (error.empty())
            {
                error = "Task intent kind does not support the requested stage";
            }
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, std::move(error));
        }

        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(_impl->Database, command.Intent.Task, task, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (const auto failure = _validateTaskMutation(_impl->Database, command.Intent.Task, task, command.Metadata, error))
        {
            return failure.value();
        }
        if (command.Intent.CreatedAtUnixMilliseconds < task->UpdatedAtUnixMilliseconds ||
            command.Intent.UpdatedAtUnixMilliseconds < task->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "Task intent creation precedes the latest Task state");
        }

        std::optional<TaskIntentRecord> currentIntent;
        if (!details::LoadTaskIntent(_impl->Database, command.Intent.Task, currentIntent, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (currentIntent && currentIntent->Stage != TaskIntentStage::Completed)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::ActiveIntentExists,
                "Task already has an incomplete intent",
                TransitionError::ActiveIntentConflict);
        }
        if (currentIntent && currentIntent->Id == command.Intent.Id)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "A new Task intent requires a new identifier");
        }
        if (command.Intent.Kind == TaskIntentKind::WorktreeCleanup)
        {
            bool hasActiveRun{};
            if (!details::HasActiveRun(_impl->Database, command.Intent.Task, hasActiveRun, error))
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
            }
            if (hasActiveRun)
            {
                return details::CommandFailure(TaskStoreCommandStatus::ActiveRunExists, "Worktree cleanup requires no active TaskRun");
            }
        }

        uint64_t eventSequence{};
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Intent.Task, eventSequence, error) ||
            !_writeNewIntent(_impl->Database, command.Intent, currentIntent.has_value(), error) ||
            !_updateTaskForIntent(
                _impl->Database,
                command.Intent.Task,
                task->Version,
                committedVersion,
                command.Metadata.TimestampUnixMilliseconds,
                TaskIntentSetsManualHold(command.Intent.Kind),
                error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Intent.Task,
                std::nullopt,
                eventSequence,
                std::nullopt,
                "TaskIntentStarted",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Intent.Task,
                StartTaskIntentCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        return _success(command.Metadata, committedVersion, eventSequence);
    }

    TaskStoreCommandResult SqliteTaskStore::AdvanceTaskIntent(const AdvanceTaskIntentCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Task, AdvanceTaskIntentCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateAdvanceTaskIntentCommand(command, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, std::move(error));
        }

        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(_impl->Database, command.Task, task, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (const auto failure = _validateTaskMutation(_impl->Database, command.Task, task, command.Metadata, error))
        {
            return failure.value();
        }

        std::optional<TaskIntentRecord> intent;
        if (!details::LoadTaskIntent(_impl->Database, command.Task, intent, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!intent || intent->Id != command.Intent)
        {
            return details::CommandFailure(TaskStoreCommandStatus::NotFound, "Task intent was not found");
        }
        if (intent->Revision != command.ExpectedIntentRevision)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::VersionConflict,
                "Task intent revision does not match expected revision",
                TransitionError::IntentRevisionMismatch);
        }
        if (command.Metadata.TimestampUnixMilliseconds < intent->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Command timestamp precedes the Task intent update time");
        }
        if (intent->Revision == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, "Task intent revision storage range is exhausted");
        }

        const auto decision = EvaluateTaskIntentTransition(intent->Kind, intent->Stage, command.RequestedStage);
        if (!decision.Allowed)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "Task intent transition was rejected", decision.Error);
        }

        uint64_t eventSequence{};
        const auto committedVersion = task->Version + 1;
        const auto committedIntentRevision = intent->Revision + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Task, eventSequence, error) ||
            !_advanceIntent(_impl->Database, command, intent.value(), committedIntentRevision, error) ||
            !details::UpdateTaskVersion(
                _impl->Database,
                command.Task,
                task->Version,
                committedVersion,
                command.Metadata.TimestampUnixMilliseconds,
                error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Task,
                std::nullopt,
                eventSequence,
                std::nullopt,
                "TaskIntentAdvanced",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task,
                AdvanceTaskIntentCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        return _success(command.Metadata, committedVersion, eventSequence);
    }
}
