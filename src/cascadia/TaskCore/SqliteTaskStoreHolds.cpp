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
        constexpr std::string_view ChangeTaskManualHoldCommandName{ "ChangeTaskManualHold" };

        bool _updateManualHold(
            sqlite3* database,
            const ChangeTaskManualHoldCommand& command,
            const uint64_t committedVersion,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE task SET manual_hold=?1, version=?2, updated_at=?3
WHERE id=?4 AND version=?5;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement ||
                !details::BindInt64(database, statement.Get(), 1, command.RequestedHold ? 1 : 0, error) ||
                !details::BindInt64(database, statement.Get(), 2, static_cast<int64_t>(committedVersion), error) ||
                !details::BindInt64(database, statement.Get(), 3, command.Metadata.TimestampUnixMilliseconds, error) ||
                !details::BindText(database, statement.Get(), 4, command.Task.Value(), error) ||
                !details::BindInt64(database, statement.Get(), 5, static_cast<int64_t>(command.Metadata.ExpectedTaskVersion), error) ||
                !details::StepDone(database, statement.Get(), error))
            {
                return false;
            }
            if (sqlite3_changes(database) != 1)
            {
                error = "Task version changed during manual-hold transaction";
                return false;
            }
            return true;
        }
    }

    TaskStoreCommandResult SqliteTaskStore::ChangeTaskManualHold(const ChangeTaskManualHoldCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Task, ChangeTaskManualHoldCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateChangeTaskManualHoldCommand(command, error))
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
        if (!command.RequestedHold)
        {
            std::optional<TaskIntentRecord> intent;
            if (!details::LoadTaskIntent(_impl->Database, command.Task, intent, error))
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
            }
            if (intent && intent->Stage != TaskIntentStage::Completed)
            {
                return details::CommandFailure(
                    TaskStoreCommandStatus::InvalidTransition,
                    "Manual hold cannot clear while a Task intent is incomplete",
                    TransitionError::ActiveIntentConflict);
            }
        }

        uint64_t eventSequence{};
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Task, eventSequence, error) ||
            !_updateManualHold(_impl->Database, command, committedVersion, error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Task,
                std::nullopt,
                eventSequence,
                std::nullopt,
                "TaskManualHoldChanged",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task,
                ChangeTaskManualHoldCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }

        TaskStoreCommandResult result;
        result.Status = TaskStoreCommandStatus::Succeeded;
        result.CommittedTaskVersion = committedVersion;
        result.LastEventSequence = eventSequence;
        result.ResultJson = command.Metadata.ReceiptResultJson;
        return result;
    }
}
