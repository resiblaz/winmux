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
        if (task->ManualHold)
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
            !details::UpdateTaskVersion(
                _impl->Database,
                command.Run.Task,
                task->Version,
                committedVersion,
                command.Metadata.TimestampUnixMilliseconds,
                error) ||
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
