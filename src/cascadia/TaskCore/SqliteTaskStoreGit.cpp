// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreGitSqlite.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view InitializeTaskGitWorktreeCommandName{
            "InitializeTaskGitWorktree"
        };
    }

    TaskStoreCommandResult SqliteTaskStore::InitializeTaskGitWorktree(
        const InitializeTaskGitWorktreeCommand& command)
    {
        if (!IsReady())
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::NotReady,
                LastError());
        }

        std::string error;
        if (!details::ValidateInitializeTaskGitWorktreeCommand(command, error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidRecord,
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
                command.Worktree.Task,
                InitializeTaskGitWorktreeCommandName))
        {
            return *replay;
        }

        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(
                _impl->Database,
                command.Worktree.Task,
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
                command.Worktree.Task,
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

        bool contextMatches{};
        if (!details::ValidateGitWorktreeExecutionContext(
                _impl->Database,
                command.Worktree,
                contextMatches,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (!contextMatches)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidRecord,
                "Frozen Worktree ExecutionContext does not match the Git Worktree identity");
        }

        bool existing{};
        if (!details::TaskHasGitWorktree(
                _impl->Database,
                command.Worktree.Task,
                existing,
                error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }
        if (existing)
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::InvalidTransition,
                "Task Git Worktree identity is already initialized");
        }

        uint64_t eventSequence{};
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(
                _impl->Database,
                command.Worktree.Task,
                eventSequence,
                error) ||
            !details::InsertTaskGitWorktree(
                _impl->Database,
                command.Worktree,
                error) ||
            !details::UpdateTaskVersion(
                _impl->Database,
                command.Worktree.Task,
                task->Version,
                committedVersion,
                command.Metadata.TimestampUnixMilliseconds,
                error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Worktree.Task,
                std::nullopt,
                eventSequence,
                std::nullopt,
                "TaskGitWorktreeInitialized",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Worktree.Task,
                InitializeTaskGitWorktreeCommandName,
                committedVersion,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(
                TaskStoreCommandStatus::StorageError,
                std::move(error));
        }

        TaskStoreCommandResult result;
        result.Status = TaskStoreCommandStatus::Succeeded;
        result.CommittedTaskVersion = committedVersion;
        result.LastEventSequence = eventSequence;
        result.ResultJson = command.Metadata.ReceiptResultJson;
        return result;
    }
}
