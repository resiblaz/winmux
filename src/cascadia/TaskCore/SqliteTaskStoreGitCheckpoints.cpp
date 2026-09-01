// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskGitCheckpointStatePolicy.h"
#include "TaskRecordValidation.h"
#include "TaskRunStatePolicy.h"
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
        constexpr std::string_view BeginCheckpointCommandName{
            "BeginTaskRunGitCheckpoint"
        };
        constexpr std::string_view AdvanceCheckpointCommandName{
            "AdvanceTaskRunGitCheckpoint"
        };

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

        bool _versionCanAdvance(
            const uint64_t version,
            std::string& error,
            const std::string_view subject)
        {
            if (version == static_cast<uint64_t>(
                               (std::numeric_limits<int64_t>::max)()))
            {
                error = std::string{ subject } + " storage range is exhausted";
                return false;
            }
            return true;
        }
    }

    TaskStoreCommandResult SqliteTaskStore::BeginTaskRunGitCheckpoint(
        const BeginTaskRunGitCheckpointCommand& command)
    {
        if (!IsReady())
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::NotReady,
                LastError());
        }

        std::string error;
        if (!details::ValidateBeginTaskRunGitCheckpointCommand(command, error))
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

        const auto& checkpoint = command.Checkpoint;
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
                checkpoint.Task,
                BeginCheckpointCommandName))
        {
            return *replay;
        }

        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(
                _impl->Database,
                checkpoint.Task,
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
                checkpoint.Task,
                error);
        }
        if (task->Version != command.Metadata.ExpectedTaskVersion)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::VersionConflict,
                "Task version does not match expected version");
        }
        if (command.Metadata.TimestampUnixMilliseconds <
                task->UpdatedAtUnixMilliseconds ||
            !_versionCanAdvance(task->Version, error, "Task version"))
        {
            return details::CommandFailure(
                error.empty() ? TaskStoreCommandStatus::InvalidCommand :
                                TaskStoreCommandStatus::StorageError,
                error.empty() ?
                    "Checkpoint timestamp precedes the Task update time" :
                    std::move(error));
        }

        std::optional<details::StoredTaskRunState> run;
        if (!details::LoadTaskRunState(
                _impl->Database,
                checkpoint.Run,
                run,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!run || run->Task != checkpoint.Task)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::NotFound,
                "TaskRun was not found for the Git checkpoint");
        }
        if (IsTaskRunTerminal(run->State))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Git checkpoint start evidence must be captured before the TaskRun ends");
        }

        std::optional<TaskGitWorktreeRecord> worktree;
        if (!details::LoadTaskGitWorktree(
                _impl->Database,
                checkpoint.Task,
                worktree,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!worktree || worktree->State != TaskGitWorktreeState::Ready)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Task Git Worktree is not ready for a TaskRun checkpoint");
        }
        bool runMatches{};
        if (!details::TaskRunUsesGitWorktree(
                _impl->Database,
                checkpoint.Task,
                checkpoint.Run,
                worktree->ExecutionContext,
                runMatches,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!runMatches)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidRecord,
                "TaskRun does not use the managed Git Worktree");
        }

        std::optional<TaskRunGitCheckpointRecord> existing;
        bool runHasCheckpoint{};
        if (!details::LoadTaskRunGitCheckpoint(
                _impl->Database,
                checkpoint.Id,
                existing,
                error) ||
            !details::TaskRunHasGitCheckpoint(
                _impl->Database,
                checkpoint.Run,
                runHasCheckpoint,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (existing || runHasCheckpoint)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidRecord,
                "TaskRun Git checkpoint already exists");
        }

        uint64_t eventSequence{};
        uint64_t runEventSequence{};
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(
                _impl->Database,
                checkpoint.Task,
                eventSequence,
                error) ||
            !details::NextRunEventSequence(
                _impl->Database,
                checkpoint.Run,
                runEventSequence,
                error) ||
            !details::InsertTaskRunGitCheckpoint(
                _impl->Database,
                checkpoint,
                error) ||
            !details::UpdateTaskVersion(
                _impl->Database,
                checkpoint.Task,
                task->Version,
                committedVersion,
                command.Metadata.TimestampUnixMilliseconds,
                error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                checkpoint.Task,
                checkpoint.Run,
                eventSequence,
                runEventSequence,
                "TaskRunGitCheckpointStarted",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                checkpoint.Task,
                BeginCheckpointCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        return _success(command.Metadata, committedVersion, eventSequence);
    }

    TaskStoreCommandResult SqliteTaskStore::AdvanceTaskRunGitCheckpoint(
        const AdvanceTaskRunGitCheckpointCommand& command)
    {
        if (!IsReady())
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::NotReady,
                LastError());
        }

        std::string error;
        if (!details::ValidateAdvanceTaskRunGitCheckpointCommand(command, error))
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
                AdvanceCheckpointCommandName))
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
                "Checkpoint timestamp precedes the Task update time");
        }
        if (!_versionCanAdvance(task->Version, error, "Task version"))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }

        std::optional<TaskRunGitCheckpointRecord> checkpoint;
        if (!details::LoadTaskRunGitCheckpoint(
                _impl->Database,
                command.Checkpoint,
                checkpoint,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!checkpoint || checkpoint->Task != command.Task ||
            checkpoint->Run != command.Run)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::NotFound,
                "TaskRun Git checkpoint was not found");
        }
        if (checkpoint->Revision != command.ExpectedCheckpointRevision)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::VersionConflict,
                "TaskRun Git checkpoint revision does not match",
                TransitionError::GitCheckpointRevisionMismatch);
        }
        if (command.Metadata.TimestampUnixMilliseconds <
                checkpoint->UpdatedAtUnixMilliseconds ||
            !_versionCanAdvance(
                checkpoint->Revision,
                error,
                "Git checkpoint revision"))
        {
            return details::CommandFailure(
                error.empty() ? TaskStoreCommandStatus::InvalidCommand :
                                TaskStoreCommandStatus::StorageError,
                error.empty() ?
                    "Checkpoint timestamp precedes its previous state" :
                    std::move(error));
        }
        const auto decision = EvaluateTaskGitCheckpointTransition(
            checkpoint->State,
            command.RequestedState);
        if (!decision.Allowed)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "TaskRun Git checkpoint transition was rejected",
                decision.Error);
        }

        std::optional<details::StoredTaskRunState> run;
        if (!details::LoadTaskRunState(
                _impl->Database,
                command.Run,
                run,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!run || run->Task != command.Task || !IsTaskRunTerminal(run->State))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "TaskRun must be terminal before its Git checkpoint advances");
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
        if (!worktree || worktree->State != TaskGitWorktreeState::Ready)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Task Git Worktree is not ready for checkpoint advancement");
        }
        if (worktree->Revision != command.ExpectedWorktreeRevision)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::VersionConflict,
                "Task Git Worktree revision does not match expected revision",
                TransitionError::GitWorktreeRevisionMismatch);
        }
        if (command.Metadata.TimestampUnixMilliseconds <
                worktree->UpdatedAtUnixMilliseconds ||
            !_versionCanAdvance(
                worktree->Revision,
                error,
                "Git Worktree revision"))
        {
            return details::CommandFailure(
                error.empty() ? TaskStoreCommandStatus::InvalidCommand :
                                TaskStoreCommandStatus::StorageError,
                error.empty() ?
                    "Checkpoint timestamp precedes the Worktree state" :
                    std::move(error));
        }

        bool runMatches{};
        if (!details::TaskRunUsesGitWorktree(
                _impl->Database,
                command.Task,
                command.Run,
                worktree->ExecutionContext,
                runMatches,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!runMatches ||
            (!command.PreservedPath.empty() &&
             command.PreservedPath != worktree->WorktreePath) ||
            (worktree->HasUndeliveredWork && !command.HasUndeliveredWork) ||
            (command.RequestedState == TaskGitCheckpointState::Inspecting &&
             command.HasUndeliveredWork != worktree->HasUndeliveredWork) ||
            (command.RequestedState == TaskGitCheckpointState::NotNeeded &&
             command.HasUndeliveredWork !=
                 (worktree->HasUndeliveredWork ||
                  command.EndCommit != worktree->BaselineCommit)) ||
            (!checkpoint->EndCommit.empty() &&
             command.RequestedState != TaskGitCheckpointState::Succeeded &&
             checkpoint->EndCommit != command.EndCommit) ||
            (checkpoint->ChangedPathsJson != "[]" &&
             checkpoint->ChangedPathsJson != command.ChangedPathsJson))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidRecord,
                "TaskRun Git checkpoint evidence changed unexpectedly");
        }

        auto nextCheckpoint = *checkpoint;
        nextCheckpoint.Revision = checkpoint->Revision + 1;
        nextCheckpoint.State = command.RequestedState;
        nextCheckpoint.EndCommit = command.EndCommit;
        nextCheckpoint.CheckpointCommit = command.CheckpointCommit;
        nextCheckpoint.AutoCheckpointed = command.AutoCheckpointed;
        nextCheckpoint.ChangedPathsJson = command.ChangedPathsJson;
        nextCheckpoint.DiffSummaryJson = command.DiffSummaryJson;
        nextCheckpoint.PreservedPath = command.PreservedPath;
        nextCheckpoint.ReasonCode = command.ReasonCode;
        nextCheckpoint.UpdatedAtUnixMilliseconds =
            command.Metadata.TimestampUnixMilliseconds;
        auto nextWorktree = *worktree;
        nextWorktree.Revision = worktree->Revision + 1;
        nextWorktree.HasUndeliveredWork = command.HasUndeliveredWork;
        nextWorktree.CleanupEligible = false;
        nextWorktree.PreservedPath = command.PreservedPath;
        nextWorktree.FailureReasonCode = command.ReasonCode;
        nextWorktree.UpdatedAtUnixMilliseconds =
            command.Metadata.TimestampUnixMilliseconds;
        if (!ValidateTaskRunGitCheckpointRecord(nextCheckpoint).empty() ||
            !ValidateTaskGitWorktreeRecord(nextWorktree).empty())
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidRecord,
                "Advanced Git checkpoint evidence is invalid");
        }

        uint64_t eventSequence{};
        uint64_t runEventSequence{};
        const auto committedVersion = task->Version + 1;
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
            !details::AdvanceTaskRunGitCheckpoint(
                _impl->Database,
                command,
                checkpoint->State,
                nextCheckpoint.Revision,
                error) ||
            !details::AdvanceGitWorktreeForCheckpoint(
                _impl->Database,
                command,
                nextWorktree.Revision,
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
                command.Run,
                eventSequence,
                runEventSequence,
                "TaskRunGitCheckpointAdvanced",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task,
                AdvanceCheckpointCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        return _success(command.Metadata, committedVersion, eventSequence);
    }
}
