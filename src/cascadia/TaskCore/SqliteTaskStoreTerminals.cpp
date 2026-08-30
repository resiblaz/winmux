// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreSqlite.h"
#include "TaskTerminalStatePolicy.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view ChangeTaskTerminalStateCommandName{ "ChangeTaskTerminalState" };

        bool _updateTerminalState(
            sqlite3* database,
            const ChangeTaskTerminalStateCommand& command,
            const details::StoredTaskTerminalState& current,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE task_terminal
SET state=?1, generation=?2, updated_at=?3
WHERE task_id=?4 AND state=?5 AND generation=?6;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement ||
                !details::BindInt64(database, statement.Get(), 1, static_cast<int64_t>(command.RequestedState), error) ||
                !details::BindInt64(database, statement.Get(), 2, static_cast<int64_t>(command.RequestedGeneration), error) ||
                !details::BindInt64(database, statement.Get(), 3, command.Metadata.TimestampUnixMilliseconds, error) ||
                !details::BindText(database, statement.Get(), 4, command.Task.Value(), error) ||
                !details::BindInt64(database, statement.Get(), 5, static_cast<int64_t>(current.State), error) ||
                !details::BindInt64(database, statement.Get(), 6, static_cast<int64_t>(current.Generation), error) ||
                !details::StepDone(database, statement.Get(), error))
            {
                return false;
            }
            if (sqlite3_changes(database) != 1)
            {
                error = "TaskTerminal state changed during transaction";
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
    }

    TaskStoreCommandResult SqliteTaskStore::ChangeTaskTerminalState(const ChangeTaskTerminalStateCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Task, ChangeTaskTerminalStateCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateTerminalStateCommand(command, error))
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

        std::optional<details::StoredTaskTerminalState> terminal;
        if (!details::LoadTaskTerminalState(_impl->Database, command.Task, terminal, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!terminal)
        {
            return details::CommandFailure(TaskStoreCommandStatus::NotFound, "TaskTerminal was not found");
        }
        if (command.Metadata.TimestampUnixMilliseconds < terminal->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Command timestamp precedes the TaskTerminal update time");
        }

        bool hasActiveRun{};
        if (!details::HasActiveRun(_impl->Database, command.Task, hasActiveRun, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        const auto decision = EvaluateTaskTerminalTransition(
            terminal->State,
            command.RequestedState,
            TaskTerminalTransitionContext{
                terminal->Generation,
                command.RequestedGeneration,
                command.ExplicitOpen,
                hasActiveRun,
            });
        if (!decision.Allowed)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "TaskTerminal transition was rejected", decision.Error);
        }

        uint64_t eventSequence{};
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Task, eventSequence, error) ||
            !_updateTerminalState(_impl->Database, command, terminal.value(), error) ||
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
                "TaskTerminalStateChanged",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task,
                ChangeTaskTerminalStateCommandName,
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
