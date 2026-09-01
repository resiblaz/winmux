// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskGitWorktreeStatePolicy.h"
#include "TaskRecordValidation.h"
#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreGitCheckpointSqlite.h"
#include "TaskStoreGitSqlite.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view FinalizationCommandName{
            "AdvanceTaskGitWorktreeFinalization"
        };

        TaskStoreCommandResult _success(
            const AdvanceTaskGitWorktreeFinalizationCommand& command,
            const uint64_t committedVersion,
            const uint64_t eventSequence)
        {
            TaskStoreCommandResult result;
            result.Status = TaskStoreCommandStatus::Succeeded;
            result.CommittedTaskVersion = committedVersion;
            result.LastEventSequence = eventSequence;
            result.ResultJson = command.Metadata.ReceiptResultJson;
            return result;
        }
    }

    TaskStoreCommandResult SqliteTaskStore::AdvanceTaskGitWorktreeFinalization(
        const AdvanceTaskGitWorktreeFinalizationCommand& command)
    {
        if (!IsReady())
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::NotReady,
                LastError());
        }
        std::string error;
        if (!details::ValidateAdvanceTaskGitWorktreeFinalizationCommand(
                command,
                error))
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
        if (const auto replay = details::EvaluateReceiptReplay(
                receipt,
                command.Metadata,
                command.Task,
                FinalizationCommandName))
        {
            return *replay;
        }

        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(
                _impl->Database,
                command.Task,
                task,
                error))
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
        if (command.Metadata.TimestampUnixMilliseconds <
                task->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidCommand,
                "Finalization timestamp precedes the Task update time");
        }
        if (task->Version == static_cast<uint64_t>(
                                 (std::numeric_limits<int64_t>::max)()))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                "Task version storage range is exhausted");
        }

        std::optional<TaskGitWorktreeRecord> worktree;
        if (!details::LoadTaskGitWorktree(
                _impl->Database,
                command.Task,
                worktree,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!worktree)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::NotFound,
                "Task Git Worktree was not initialized");
        }
        if (worktree->ExecutionContext != command.ExecutionContext)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidRecord,
                "Task Git Worktree ExecutionContext does not match");
        }
        if (worktree->Revision != command.ExpectedWorktreeRevision)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::VersionConflict,
                "Task Git Worktree revision does not match expected revision",
                TransitionError::GitWorktreeRevisionMismatch);
        }
        if (command.Metadata.TimestampUnixMilliseconds <
                worktree->UpdatedAtUnixMilliseconds)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidCommand,
                "Finalization timestamp precedes the Worktree update time");
        }
        if (worktree->Revision == static_cast<uint64_t>(
                                     (std::numeric_limits<int64_t>::max)()))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                "Task Git Worktree revision storage range is exhausted");
        }
        const auto decision = EvaluateTaskGitWorktreeFinalizationTransition(
            worktree->State,
            command.RequestedState);
        if (!decision.Allowed)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Task Git Worktree finalization transition was rejected",
                decision.Error);
        }

        bool activeRun{};
        bool incompleteCheckpoint{};
        if (!details::HasActiveRun(
                _impl->Database,
                command.Task,
                activeRun,
                error) ||
            !details::TaskHasIncompleteGitCheckpoint(
                _impl->Database,
                command.Task,
                incompleteCheckpoint,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (activeRun)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::ActiveRunExists,
                "Task Git Worktree cannot finalize while a TaskRun is active",
                TransitionError::ActiveRunConflict);
        }
        if (incompleteCheckpoint)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Task Git Worktree cannot finalize before checkpoint recovery",
                TransitionError::UndeliveredWork);
        }
        if (worktree->SnapshotDigest.empty() || worktree->BaselineCommit.empty() ||
            (!command.PreservedPath.empty() &&
             command.PreservedPath != worktree->WorktreePath) ||
            (command.RequestedState == TaskGitWorktreeState::Finalizing &&
             command.HasUndeliveredWork != worktree->HasUndeliveredWork))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidRecord,
                "Task Git Worktree finalization evidence is inconsistent");
        }

        auto next = *worktree;
        next.Revision = worktree->Revision + 1;
        next.State = command.RequestedState;
        next.HasUndeliveredWork = command.HasUndeliveredWork;
        next.CleanupEligible = command.CleanupEligible;
        next.PreservedPath = command.PreservedPath;
        next.FailureReasonCode = command.FailureReasonCode;
        next.UpdatedAtUnixMilliseconds =
            command.Metadata.TimestampUnixMilliseconds;
        if (!ValidateTaskGitWorktreeRecord(next).empty())
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidRecord,
                "Finalized Task Git Worktree record is invalid");
        }

        uint64_t eventSequence{};
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(
                _impl->Database,
                command.Task,
                eventSequence,
                error) ||
            !details::AdvanceTaskGitWorktreeFinalization(
                _impl->Database,
                command,
                worktree->State,
                next.Revision,
                error) ||
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
                "TaskGitWorktreeFinalizationAdvanced",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task,
                FinalizationCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        return _success(command, committedVersion, eventSequence);
    }
}
