// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreRunSqlite.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view CreateTaskRunCommandName{ "CreateTaskRun" };

        bool _updateTaskForRun(
            sqlite3* database,
            const CreateTaskRunCommand& command,
            const uint64_t currentVersion,
            const uint64_t committedVersion,
            std::string& error)
        {
            if (!command.ClearManualHold)
            {
                return details::UpdateTaskVersion(
                    database, command.Run.Task, currentVersion, committedVersion,
                    command.Metadata.TimestampUnixMilliseconds, error);
            }
            constexpr auto sql = R"SQL(
UPDATE task SET manual_hold=0, version=?1, updated_at=?2
WHERE id=?3 AND version=?4;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement ||
                !details::BindInt64(database, statement.Get(), 1, static_cast<int64_t>(committedVersion), error) ||
                !details::BindInt64(database, statement.Get(), 2, command.Metadata.TimestampUnixMilliseconds, error) ||
                !details::BindText(database, statement.Get(), 3, command.Run.Task.Value(), error) ||
                !details::BindInt64(database, statement.Get(), 4, static_cast<int64_t>(currentVersion), error) ||
                !details::StepDone(database, statement.Get(), error))
            {
                return false;
            }
            if (sqlite3_changes(database) != 1)
            {
                error = "Task changed while clearing manual hold for a Run";
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

        bool _timestampIsValid(
            const TaskCommandMetadata& metadata,
            const details::StoredTaskState& task,
            std::string& error)
        {
            if (metadata.TimestampUnixMilliseconds < task.UpdatedAtUnixMilliseconds)
            {
                error = "Command timestamp precedes the Task update time";
                return false;
            }
            return true;
        }

        bool _versionCanAdvance(const details::StoredTaskState& task, std::string& error)
        {
            if (task.Version == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            {
                error = "Task version storage range is exhausted";
                return false;
            }
            return true;
        }
    }

    TaskStoreCommandResult SqliteTaskStore::CreateTaskRun(const CreateTaskRunCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Run.Task, CreateTaskRunCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateCreateTaskRunCommand(command, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, std::move(error));
        }

        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(_impl->Database, command.Run.Task, task, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!task)
        {
            return details::MissingTaskFailure(_impl->Database, command.Run.Task, error);
        }
        if (task->Version != command.Metadata.ExpectedTaskVersion)
        {
            return details::CommandFailure(TaskStoreCommandStatus::VersionConflict, "Task version does not match expected version");
        }
        if (!_timestampIsValid(command.Metadata, task.value(), error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, std::move(error));
        }
        if (!_versionCanAdvance(task.value(), error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (command.Run.CreatedAtUnixMilliseconds < task->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "TaskRun creation precedes the latest Task state");
        }
        if (task->ManualHold && !command.ClearManualHold)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "TaskRun cannot start while the Task is on manual hold",
                TransitionError::ManualHoldActive);
        }
        if (task->Status == TaskStatus::Done || task->Status == TaskStatus::Cancelled)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "Terminal Task status must be reopened before a TaskRun starts");
        }
        std::optional<TaskIntentRecord> currentIntent;
        if (!details::LoadTaskIntent(_impl->Database, command.Run.Task, currentIntent, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (currentIntent && currentIntent->Stage != TaskIntentStage::Completed)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "TaskRun cannot start while a Task intent is incomplete",
                TransitionError::ActiveIntentConflict);
        }

        std::optional<details::StoredTaskRunState> existingRun;
        if (!details::LoadTaskRunState(_impl->Database, command.Run.Id, existingRun, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (existingRun)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "TaskRun already exists");
        }
        const auto references = details::ValidateTaskRunReferences(_impl->Database, command.Run, error);
        if (references == details::TaskRunReferenceValidation::StorageError)
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (references == details::TaskRunReferenceValidation::Invalid)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, std::move(error));
        }

        bool hasActiveRun{};
        if (!details::HasActiveRun(_impl->Database, command.Run.Task, hasActiveRun, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (hasActiveRun)
        {
            return details::CommandFailure(TaskStoreCommandStatus::ActiveRunExists, "Task already has an active TaskRun");
        }

        uint64_t runSequence{};
        uint64_t eventSequence{};
        constexpr uint64_t runEventSequence{ 1 };
        const auto committedVersion = task->Version + 1;
        if (!details::NextRunSequence(_impl->Database, command.Run.Task, runSequence, error) ||
            !details::NextAggregateSequence(_impl->Database, command.Run.Task, eventSequence, error) ||
            !details::InsertTaskRun(_impl->Database, command.Run, runSequence, error) ||
            !_updateTaskForRun(
                _impl->Database, command, task->Version, committedVersion, error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Run.Task,
                command.Run.Id,
                eventSequence,
                runEventSequence,
                "TaskRunCreated",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Run.Task,
                CreateTaskRunCommandName,
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
