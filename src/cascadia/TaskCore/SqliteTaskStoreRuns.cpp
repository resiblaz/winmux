// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view CreateTaskRunCommandName{ "CreateTaskRun" };

        std::optional<bool> _referenceBelongsToTask(
            sqlite3* database,
            const char* sql,
            const TaskId& task,
            const std::string_view id,
            std::string& error)
        {
            bool belongs{};
            if (!details::RowBelongsToTask(database, sql, task, id, belongs, error))
            {
                return std::nullopt;
            }
            return belongs;
        }

        std::optional<bool> _validateRunReferences(sqlite3* database, const TaskRunRecord& run, std::string& error)
        {
            const auto executionContext = _referenceBelongsToTask(
                database,
                "SELECT EXISTS(SELECT 1 FROM execution_context WHERE id=?1 AND task_id=?2);",
                run.Task,
                run.ExecutionContext.Value(),
                error);
            if (!executionContext)
            {
                return std::nullopt;
            }
            if (!executionContext.value())
            {
                error = "ExecutionContext does not belong to the Task";
                return false;
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
                    return std::nullopt;
                }
                if (!agentSession.value())
                {
                    error = "AgentSession does not belong to the Task";
                    return false;
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
                    return std::nullopt;
                }
                if (!retryRun.value())
                {
                    error = "Retry source TaskRun does not belong to the Task";
                    return false;
                }
            }
            return true;
        }

        bool _insertRun(
            sqlite3* database,
            const TaskRunRecord& run,
            const uint64_t sequence,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
INSERT INTO task_run(id, task_id, execution_context_id, agent_session_id, retry_of_run_id,
                     trigger, state, sequence, created_at, started_at, finished_at, result, reason_code)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, NULL, NULL, ?10, ?11);
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            const auto agentSession = run.AgentSession ?
                                          std::optional<std::string>{ run.AgentSession->Value() } :
                                          std::nullopt;
            const auto retryOf = run.RetryOfRunId ?
                                     std::optional<std::string>{ run.RetryOfRunId->Value() } :
                                     std::nullopt;
            return statement &&
                   details::BindText(database, statement.Get(), 1, run.Id.Value(), error) &&
                   details::BindText(database, statement.Get(), 2, run.Task.Value(), error) &&
                   details::BindText(database, statement.Get(), 3, run.ExecutionContext.Value(), error) &&
                   details::BindOptionalText(database, statement.Get(), 4, agentSession, error) &&
                   details::BindOptionalText(database, statement.Get(), 5, retryOf, error) &&
                   details::BindInt64(database, statement.Get(), 6, static_cast<int64_t>(run.Trigger), error) &&
                   details::BindInt64(database, statement.Get(), 7, static_cast<int64_t>(run.State), error) &&
                   details::BindInt64(database, statement.Get(), 8, static_cast<int64_t>(sequence), error) &&
                   details::BindInt64(database, statement.Get(), 9, run.CreatedAtUnixMilliseconds, error) &&
                   details::BindText(database, statement.Get(), 10, run.Result, error) &&
                   details::BindText(database, statement.Get(), 11, run.ReasonCode, error) &&
                   details::StepDone(database, statement.Get(), error);
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
        const auto referencesValid = _validateRunReferences(_impl->Database, command.Run, error);
        if (!referencesValid)
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!referencesValid.value())
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
            !_insertRun(_impl->Database, command.Run, runSequence, error) ||
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
