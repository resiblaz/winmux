// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskRunStatePolicy.h"
#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view CreateTaskRunCommandName{ "CreateTaskRun" };
        constexpr std::string_view ChangeTaskRunStateCommandName{ "ChangeTaskRunState" };

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

        bool _updateRunState(
            sqlite3* database,
            const ChangeTaskRunStateCommand& command,
            const details::StoredTaskRunState& current,
            std::string& error)
        {
            auto startedAt = current.StartedAtUnixMilliseconds;
            auto finishedAt = current.FinishedAtUnixMilliseconds;
            auto result = current.Result;
            auto reasonCode = current.ReasonCode;
            if (command.RequestedState == TaskRunState::Running && !startedAt)
            {
                startedAt = command.Metadata.TimestampUnixMilliseconds;
            }
            if (IsTaskRunTerminal(command.RequestedState))
            {
                finishedAt = command.Metadata.TimestampUnixMilliseconds;
                result = command.Result;
                reasonCode = command.ReasonCode;
            }

            constexpr auto sql = R"SQL(
UPDATE task_run
SET state=?1, started_at=?2, finished_at=?3, result=?4, reason_code=?5
WHERE id=?6 AND task_id=?7 AND state=?8;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            return statement &&
                   details::BindInt64(database, statement.Get(), 1, static_cast<int64_t>(command.RequestedState), error) &&
                   details::BindOptionalInt64(database, statement.Get(), 2, startedAt, error) &&
                   details::BindOptionalInt64(database, statement.Get(), 3, finishedAt, error) &&
                   details::BindText(database, statement.Get(), 4, result, error) &&
                   details::BindText(database, statement.Get(), 5, reasonCode, error) &&
                   details::BindText(database, statement.Get(), 6, command.Run.Value(), error) &&
                   details::BindText(database, statement.Get(), 7, command.Task.Value(), error) &&
                   details::BindInt64(database, statement.Get(), 8, static_cast<int64_t>(current.State), error) &&
                   details::StepDone(database, statement.Get(), error) &&
                   sqlite3_changes(database) == 1;
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
            return details::CommandFailure(TaskStoreCommandStatus::NotFound, "Task was not found");
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
        if (command.Run.CreatedAtUnixMilliseconds < task->CreatedAtUnixMilliseconds)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "TaskRun creation precedes its Task");
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

    TaskStoreCommandResult SqliteTaskStore::ChangeTaskRunState(const ChangeTaskRunStateCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Task, ChangeTaskRunStateCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateRunStateCommand(command, error))
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
        if (!_timestampIsValid(command.Metadata, task.value(), error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, std::move(error));
        }
        if (!_versionCanAdvance(task.value(), error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }

        std::optional<details::StoredTaskRunState> run;
        if (!details::LoadTaskRunState(_impl->Database, command.Run, run, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!run || run->Task != command.Task)
        {
            return details::CommandFailure(TaskStoreCommandStatus::NotFound, "TaskRun was not found for the Task");
        }
        if (command.Metadata.TimestampUnixMilliseconds < run->CreatedAtUnixMilliseconds ||
            (run->StartedAtUnixMilliseconds && command.Metadata.TimestampUnixMilliseconds < run->StartedAtUnixMilliseconds.value()))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Command timestamp precedes the TaskRun state");
        }
        if (IsTaskRunTerminal(run->State) &&
            (command.Result != run->Result || command.ReasonCode != run->ReasonCode))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "Terminal TaskRun outcome is immutable", TransitionError::InvalidTransition);
        }

        const auto decision = EvaluateTaskRunTransition(run->State, command.RequestedState);
        if (!decision.Allowed)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "TaskRun transition was rejected", decision.Error);
        }

        uint64_t eventSequence{};
        uint64_t runEventSequence{};
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Task, eventSequence, error) ||
            !details::NextRunEventSequence(_impl->Database, command.Run, runEventSequence, error) ||
            (decision.Changed && !_updateRunState(_impl->Database, command, run.value(), error)) ||
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
                command.Run,
                eventSequence,
                runEventSequence,
                "TaskRunStateChanged",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task,
                ChangeTaskRunStateCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            if (error.empty())
            {
                error = "TaskRun state changed during transaction";
            }
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }

        return _success(command.Metadata, committedVersion, eventSequence);
    }

}
