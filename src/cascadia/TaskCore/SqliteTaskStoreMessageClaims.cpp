// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreMessageSqlite.h"
#include "TaskStoreRunSqlite.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view ClaimTaskMessagesCommandName{ "ClaimTaskMessagesAndCreateRun" };
    }

    TaskStoreCommandResult SqliteTaskStore::ClaimTaskMessagesAndCreateRun(
        const ClaimTaskMessagesAndCreateRunCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(
                receipt,
                command.Metadata,
                command.Run.Task,
                ClaimTaskMessagesCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateClaimTaskMessagesCommand(command, error))
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
        if (command.Metadata.TimestampUnixMilliseconds < task->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Command timestamp precedes the Task update time");
        }
        if (task->Version == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, "Task version storage range is exhausted");
        }
        if (task->ManualHold)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Task messages cannot be claimed while the Task is on manual hold",
                TransitionError::ManualHoldActive);
        }
        if (task->Status == TaskStatus::Done || task->Status == TaskStatus::Cancelled)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "Task must be reopened before messages create a TaskRun");
        }
        if (command.Run.CreatedAtUnixMilliseconds < task->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "Follow-up TaskRun creation precedes the latest Task state");
        }

        std::optional<TaskIntentRecord> intent;
        if (!details::LoadTaskIntent(_impl->Database, command.Run.Task, intent, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (intent && intent->Stage != TaskIntentStage::Completed)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Task messages cannot be claimed while a Task intent is incomplete",
                TransitionError::ActiveIntentConflict);
        }

        std::optional<details::StoredActiveTaskRun> activeRun;
        if (!details::LoadActiveTaskRun(_impl->Database, command.Run.Task, activeRun, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (activeRun)
        {
            return details::CommandFailure(TaskStoreCommandStatus::ActiveRunExists, "Task already has an active TaskRun");
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

        std::vector<TaskMessageRecord> messages;
        bool exceedsMaximum{};
        if (!details::LoadClaimableTaskMessages(
                _impl->Database,
                command.Run.Task,
                MaximumTaskMessagesPerRun,
                messages,
                exceedsMaximum,
                error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (messages.empty())
        {
            return details::CommandFailure(TaskStoreCommandStatus::NotFound, "Task has no unclaimed messages");
        }
        if (exceedsMaximum)
        {
            return details::CommandFailure(TaskStoreCommandStatus::CapacityExceeded, "Task has too many messages for one follow-up TaskRun");
        }
        if (std::ranges::any_of(messages, [](const auto& message) {
                return message.Author != TaskMessageAuthor::User ||
                       message.Content.empty() ||
                       message.Content.size() > MaximumTaskMessageBytes;
            }))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "Claimable Task message content is invalid");
        }
        if (std::ranges::any_of(messages, [&](const auto& message) {
                return message.UpdatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds;
            }))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Command timestamp precedes a claimed Task message");
        }

        uint64_t runSequence{};
        uint64_t eventSequence{};
        constexpr uint64_t runEventSequence{ 1 };
        const auto committedVersion = task->Version + 1;
        if (!details::NextRunSequence(_impl->Database, command.Run.Task, runSequence, error) ||
            !details::NextAggregateSequence(_impl->Database, command.Run.Task, eventSequence, error) ||
            !details::InsertTaskRun(_impl->Database, command.Run, runSequence, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        for (size_t position = 0; position < messages.size(); ++position)
        {
            if (!details::ClaimTaskMessage(
                    _impl->Database,
                    messages[position],
                    command.Run.Id,
                    command.Metadata.TimestampUnixMilliseconds,
                    error) ||
                !details::InsertTaskRunMessage(
                    _impl->Database,
                    command.Run.Id,
                    messages[position],
                    position,
                    error))
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
            }
        }

        if (!details::UpdateTaskVersion(
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
                "TaskMessagesClaimedForRun",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Run.Task,
                ClaimTaskMessagesCommandName,
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
