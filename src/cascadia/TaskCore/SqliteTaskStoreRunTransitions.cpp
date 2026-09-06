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
        constexpr std::string_view ChangeTaskRunStateCommandName{ "ChangeTaskRunState" };

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
                if (!command.Result.empty())
                {
                    result = command.Result;
                }
                reasonCode = command.ReasonCode;
            }

            constexpr auto sql = R"SQL(
UPDATE task_run
SET state=?1, started_at=?2, finished_at=?3, result=?4, reason_code=?5,
    agent_session_id=COALESCE(?9, agent_session_id)
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
                   details::BindOptionalText(database, statement.Get(), 9,
                                             command.AgentSession ? std::optional{ command.AgentSession->Value() } : std::nullopt, error) &&
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
            return details::MissingTaskFailure(_impl->Database, command.Task, error);
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
        if (command.AgentSession)
        {
            if (run->AgentSession && run->AgentSession != command.AgentSession)
            {
                return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "TaskRun AgentSession binding is immutable", TransitionError::InvalidTransition);
            }
            details::SqliteStatement session{ _impl->Database,
                                             "SELECT 1 FROM agent_session WHERE id=?1 AND task_id=?2;", error };
            if (!session ||
                !details::BindText(_impl->Database, session.Get(), 1, command.AgentSession->Value(), error) ||
                !details::BindText(_impl->Database, session.Get(), 2, command.Task.Value(), error))
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
            }
            const auto step = sqlite3_step(session.Get());
            if (step == SQLITE_DONE)
            {
                return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "AgentSession does not belong to the Task");
            }
            if (step != SQLITE_ROW)
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError,
                                               details::SqliteError(_impl->Database, step, "validate TaskRun AgentSession"));
            }
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
            ((decision.Changed || (command.AgentSession && !run->AgentSession)) &&
             !_updateRunState(_impl->Database, command, run.value(), error)) ||
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
