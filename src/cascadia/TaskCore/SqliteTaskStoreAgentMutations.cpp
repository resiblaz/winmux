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
        constexpr std::string_view ApplyTaskAgentMutationCommandName{
            "ApplyTaskAgentMutation"
        };

        std::optional<TaskStatus> _requestedStatus(
            const TaskAgentMutationKind kind) noexcept
        {
            switch (kind)
            {
            case TaskAgentMutationKind::StatusInProgress:
                return TaskStatus::InProgress;
            case TaskAgentMutationKind::StatusInReview:
                return TaskStatus::InReview;
            case TaskAgentMutationKind::StatusBlocked:
                return TaskStatus::Blocked;
            default:
                return std::nullopt;
            }
        }

        bool _updateStatus(
            sqlite3* database,
            const ApplyTaskAgentMutationCommand& command,
            const uint64_t expectedVersion,
            const TaskStatus status,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE task SET status=?1 WHERE id=?2 AND version=?3;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            return statement &&
                   details::BindInt64(
                       database,
                       statement.Get(),
                       1,
                       static_cast<int64_t>(status),
                       error) &&
                   details::BindText(
                       database,
                       statement.Get(),
                       2,
                       command.Task.Value(),
                       error) &&
                   details::BindInt64(
                       database,
                       statement.Get(),
                       3,
                       static_cast<int64_t>(expectedVersion),
                       error) &&
                   details::StepDone(database, statement.Get(), error) &&
                   sqlite3_changes(database) == 1;
        }

        bool _updateResult(
            sqlite3* database,
            const ApplyTaskAgentMutationCommand& command,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE task_run SET result=?1
WHERE id=?2 AND task_id=?3 AND state IN (?4, ?5);
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            return statement &&
                   details::BindText(
                       database,
                       statement.Get(),
                       1,
                       command.Text,
                       error) &&
                   details::BindText(
                       database,
                       statement.Get(),
                       2,
                       command.Run.Value(),
                       error) &&
                   details::BindText(
                       database,
                       statement.Get(),
                       3,
                       command.Task.Value(),
                       error) &&
                   details::BindInt64(
                       database,
                       statement.Get(),
                       4,
                       static_cast<int64_t>(TaskRunState::Starting),
                       error) &&
                   details::BindInt64(
                       database,
                       statement.Get(),
                       5,
                       static_cast<int64_t>(TaskRunState::Running),
                       error) &&
                   details::StepDone(database, statement.Get(), error) &&
                   sqlite3_changes(database) == 1;
        }

        std::optional<TaskStoreCommandResult> _replay(
            const std::optional<details::StoredReceipt>& receipt,
            const ApplyTaskAgentMutationCommand& command)
        {
            if (!receipt)
            {
                return std::nullopt;
            }
            if (receipt->Task != command.Task ||
                receipt->CommandName != ApplyTaskAgentMutationCommandName ||
                receipt->RequestDigest != command.Metadata.RequestDigest)
            {
                return details::CommandFailure(
                    TaskStoreCommandStatus::IdempotencyConflict,
                    "Command ID was already used for a different Agent Task mutation");
            }

            TaskStoreCommandResult result;
            result.Status = TaskStoreCommandStatus::Replayed;
            result.CommittedTaskVersion = receipt->CommittedVersion;
            result.LastEventSequence = receipt->LastEventSequence;
            result.ResultJson = receipt->ResultJson;
            return result;
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

    TaskStoreCommandResult SqliteTaskStore::ApplyTaskAgentMutation(
        const ApplyTaskAgentMutationCommand& command)
    {
        if (!IsReady())
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::NotReady,
                LastError());
        }

        std::string error;
        if (!details::ValidateTaskAgentMutationCommand(command, error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidCommand,
                std::move(error));
        }

        std::scoped_lock lock{ _impl->DatabaseMutex };
        details::SqliteTransaction transaction{ _impl->Database, error };
        if (!transaction)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }

        std::optional<details::StoredReceipt> receipt;
        if (!details::LookupReceipt(
                _impl->Database,
                command.Metadata.Command,
                receipt,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (const auto replay = _replay(receipt, command))
        {
            return *replay;
        }

        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(_impl->Database, command.Task, task, error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!task)
        {
            return details::MissingTaskFailure(
                _impl->Database,
                command.Task,
                error);
        }
        if (task->Version != command.Metadata.ExpectedTaskVersion)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::VersionConflict,
                "Task version does not match expected version");
        }
        if (command.Metadata.TimestampUnixMilliseconds < task->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidCommand,
                "Command timestamp precedes the Task update time");
        }
        if (task->Version == static_cast<uint64_t>(
                                 (std::numeric_limits<int64_t>::max)()))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                "Task version storage range is exhausted");
        }

        std::optional<details::StoredTaskRunState> run;
        if (!details::LoadTaskRunState(_impl->Database, command.Run, run, error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!run || run->Task != command.Task)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::NotFound,
                "Active TaskRun was not found for the capability-bound Task");
        }
        if (run->State != TaskRunState::Starting &&
            run->State != TaskRunState::Running)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Agent Task mutation requires a starting or running TaskRun",
                TransitionError::InvalidTransition);
        }
        if (command.Metadata.TimestampUnixMilliseconds < run->CreatedAtUnixMilliseconds ||
            (run->StartedAtUnixMilliseconds &&
             command.Metadata.TimestampUnixMilliseconds < *run->StartedAtUnixMilliseconds))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidCommand,
                "Command timestamp precedes the active TaskRun");
        }

        std::optional<TaskIntentRecord> currentIntent;
        if (!details::LoadTaskIntent(
                _impl->Database,
                command.Task,
                currentIntent,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (currentIntent && currentIntent->Stage != TaskIntentStage::Completed)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Agent Task mutation is blocked by an incomplete Task intent",
                TransitionError::ActiveIntentConflict);
        }

        const auto requestedStatus = _requestedStatus(command.Kind);
        StateTransitionDecision<TaskStatus> statusDecision;
        if (requestedStatus)
        {
            TaskStatusTransitionContext context;
            if (!details::HasActiveRun(
                    _impl->Database,
                    command.Task,
                    context.HasActiveRun,
                    error) ||
                !details::HasUnclaimedMessages(
                    _impl->Database,
                    command.Task,
                    context.HasUnclaimedMessages,
                    error) ||
                !details::HasPendingAutomaticRetry(
                    _impl->Database,
                    command.Task,
                    context.HasPendingAutomaticRetry,
                    error))
            {
                return details::CommandFailure(
                    TaskStoreCommandStatus::StorageError,
                    std::move(error));
            }
            statusDecision = EvaluateTaskStatusTransition(
                task->Status,
                *requestedStatus,
                TaskStatusAuthority::Agent,
                context);
            if (!statusDecision.Allowed)
            {
                return details::CommandFailure(
                    TaskStoreCommandStatus::InvalidTransition,
                    "Agent Task status transition was rejected",
                    statusDecision.Error);
            }
        }

        uint64_t eventSequence{};
        uint64_t runEventSequence{};
        const auto committedVersion = task->Version + 1;
        const auto mutationSucceeded = requestedStatus ?
                                           (!statusDecision.Changed ||
                                            _updateStatus(
                                                _impl->Database,
                                                command,
                                                task->Version,
                                                *requestedStatus,
                                                error)) :
                                           _updateResult(
                                               _impl->Database,
                                               command,
                                               error);
        if (!details::NextAggregateSequence(
                _impl->Database,
                command.Task,
                eventSequence,
                error) ||
            !details::NextRunEventSequence(
                _impl->Database,
                command.Run,
                runEventSequence,
                error) ||
            !mutationSucceeded ||
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
                requestedStatus ?
                    "AgentTaskStatusChanged" :
                    "TaskRunResultUpdated",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task,
                ApplyTaskAgentMutationCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            if (error.empty())
            {
                error = "Agent Task mutation lost its compare-and-swap race";
            }
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }

        return _success(command.Metadata, committedVersion, eventSequence);
    }
}
