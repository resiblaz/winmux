// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreMessageSqlite.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view EnqueueTaskMessageCommandName{ "EnqueueTaskMessage" };
        constexpr std::string_view ReviseTaskMessageCommandName{ "ReviseTaskMessage" };

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

        bool _reviseMessage(
            sqlite3* database,
            const ReviseTaskMessageCommand& command,
            const TaskMessageRecord& current,
            const uint64_t committedRevision,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE task_message
SET state=?1, revision=?2, content=?3, updated_at=?4
WHERE id=?5 AND task_id=?6 AND revision=?7 AND state=?8 AND claimed_by_run_id IS NULL;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            const auto state = command.Remove ? TaskMessageState::Removed : current.State;
            const auto content = command.Remove ? std::string_view{} : std::string_view{ command.ReplacementContent.value() };
            if (!statement ||
                !details::BindInt64(database, statement.Get(), 1, static_cast<int64_t>(state), error) ||
                !details::BindInt64(database, statement.Get(), 2, static_cast<int64_t>(committedRevision), error) ||
                !details::BindText(database, statement.Get(), 3, content, error) ||
                !details::BindInt64(database, statement.Get(), 4, command.Metadata.TimestampUnixMilliseconds, error) ||
                !details::BindText(database, statement.Get(), 5, command.Message.Value(), error) ||
                !details::BindText(database, statement.Get(), 6, command.Task.Value(), error) ||
                !details::BindInt64(database, statement.Get(), 7, static_cast<int64_t>(command.ExpectedMessageRevision), error) ||
                !details::BindInt64(database, statement.Get(), 8, static_cast<int64_t>(current.State), error) ||
                !details::StepDone(database, statement.Get(), error))
            {
                return false;
            }
            if (sqlite3_changes(database) != 1)
            {
                error = "Task message changed during revision transaction";
                return false;
            }
            return true;
        }
    }

    TaskStoreCommandResult SqliteTaskStore::EnqueueTaskMessage(const EnqueueTaskMessageCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Message.Task, EnqueueTaskMessageCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateEnqueueTaskMessageCommand(command, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, std::move(error));
        }

        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(_impl->Database, command.Message.Task, task, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (const auto failure = _validateTaskMutation(_impl->Database, command.Message.Task, task, command.Metadata, error))
        {
            return failure.value();
        }
        if (task->Status == TaskStatus::Done || task->Status == TaskStatus::Cancelled)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "Task must be reopened before accepting a follow-up message");
        }
        if (command.Message.CreatedAtUnixMilliseconds < task->UpdatedAtUnixMilliseconds ||
            command.Message.UpdatedAtUnixMilliseconds < task->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "Task message creation precedes the latest Task state");
        }

        std::optional<TaskMessageRecord> existing;
        if (!details::LoadTaskMessage(_impl->Database, command.Message.Id, existing, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (existing)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "Task message already exists");
        }

        std::optional<details::StoredActiveTaskRun> activeRun;
        std::optional<TaskIntentRecord> intent;
        if (!details::LoadActiveTaskRun(_impl->Database, command.Message.Task, activeRun, error) ||
            !details::LoadTaskIntent(_impl->Database, command.Message.Task, intent, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        const auto intentIncomplete = intent && intent->Stage != TaskIntentStage::Completed;
        const auto foldsIntoRun = activeRun && !task->ManualHold && !intentIncomplete &&
                                  (activeRun->State == TaskRunState::Queued ||
                                   activeRun->State == TaskRunState::Preparing ||
                                   activeRun->State == TaskRunState::WaitingForDirectory);
        const auto finalState = foldsIntoRun ?
                                    TaskMessageState::Claimed :
                                    (activeRun || task->ManualHold || intentIncomplete ?
                                         TaskMessageState::Deferred :
                                         TaskMessageState::Pending);
        if (finalState != TaskMessageState::Claimed)
        {
            uint64_t unclaimedCount{};
            if (!details::CountUnclaimedTaskMessages(_impl->Database, command.Message.Task, unclaimedCount, error))
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
            }
            if (unclaimedCount >= MaximumUnclaimedTaskMessages)
            {
                return details::CommandFailure(TaskStoreCommandStatus::CapacityExceeded, "Task has reached the unclaimed message limit");
            }
        }

        uint64_t eventSequence{};
        std::optional<uint64_t> runEventSequence;
        std::optional<TaskRunId> claimedRun;
        uint64_t runMessagePosition{};
        if (foldsIntoRun)
        {
            claimedRun = activeRun->Id;
            uint64_t sequence{};
            if (!details::NextRunEventSequence(_impl->Database, activeRun->Id, sequence, error) ||
                !details::NextRunMessagePosition(_impl->Database, activeRun->Id, runMessagePosition, error))
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
            }
            if (runMessagePosition >= MaximumTaskMessagesPerRun)
            {
                return details::CommandFailure(TaskStoreCommandStatus::CapacityExceeded, "TaskRun has reached the folded message limit");
            }
            runEventSequence = sequence;
        }

        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Message.Task, eventSequence, error) ||
            !details::InsertTaskMessage(_impl->Database, command.Message, finalState, claimedRun, error) ||
            (foldsIntoRun && !details::InsertTaskRunMessage(
                                  _impl->Database,
                                  activeRun->Id,
                                  command.Message,
                                  runMessagePosition,
                                  error)) ||
            !details::UpdateTaskVersion(
                _impl->Database,
                command.Message.Task,
                task->Version,
                committedVersion,
                command.Metadata.TimestampUnixMilliseconds,
                error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Message.Task,
                claimedRun,
                eventSequence,
                runEventSequence,
                "TaskMessageEnqueued",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Message.Task,
                EnqueueTaskMessageCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        return _success(command.Metadata, committedVersion, eventSequence);
    }

    TaskStoreCommandResult SqliteTaskStore::ReviseTaskMessage(const ReviseTaskMessageCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Task, ReviseTaskMessageCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateReviseTaskMessageCommand(command, error))
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

        std::optional<TaskMessageRecord> message;
        if (!details::LoadTaskMessage(_impl->Database, command.Message, message, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!message || message->Task != command.Task)
        {
            return details::CommandFailure(TaskStoreCommandStatus::NotFound, "Task message was not found");
        }
        if (message->Revision != command.ExpectedMessageRevision)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::VersionConflict,
                "Task message revision does not match expected revision",
                TransitionError::MessageRevisionMismatch);
        }
        if ((message->State != TaskMessageState::Pending && message->State != TaskMessageState::Deferred) ||
            message->ClaimedByRunId)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "Claimed or removed Task messages are immutable");
        }
        if (command.Metadata.TimestampUnixMilliseconds < message->UpdatedAtUnixMilliseconds ||
            message->Revision == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Task message revision timestamp or range is invalid");
        }

        uint64_t eventSequence{};
        const auto committedTaskVersion = task->Version + 1;
        const auto committedMessageRevision = message->Revision + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Task, eventSequence, error) ||
            !_reviseMessage(_impl->Database, command, message.value(), committedMessageRevision, error) ||
            !details::UpdateTaskVersion(
                _impl->Database,
                command.Task,
                task->Version,
                committedTaskVersion,
                command.Metadata.TimestampUnixMilliseconds,
                error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Task,
                std::nullopt,
                eventSequence,
                std::nullopt,
                command.Remove ? "TaskMessageRemoved" : "TaskMessageRevised",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task,
                ReviseTaskMessageCommandName,
                committedTaskVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        return _success(command.Metadata, committedTaskVersion, eventSequence);
    }
}
