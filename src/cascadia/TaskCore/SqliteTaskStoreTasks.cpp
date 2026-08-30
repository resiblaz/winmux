// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStatusPolicy.h"
#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreSqlite.h"

#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view CreateTaskCommandName{ "CreateTask" };
        constexpr std::string_view ChangeTaskStatusCommandName{ "ChangeTaskStatus" };

        bool _insertTask(sqlite3* database, const TaskRecord& task, std::string& error)
        {
            constexpr auto sql = R"SQL(
INSERT INTO task(id, workspace_id, forked_from_task_id, title, description, status, priority,
                 version, manual_hold, created_at, updated_at)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            const auto forkedFrom = task.ForkedFromTaskId ?
                                        std::optional<std::string>{ task.ForkedFromTaskId->Value() } :
                                        std::nullopt;
            return statement &&
                   details::BindText(database, statement.Get(), 1, task.Id.Value(), error) &&
                   details::BindText(database, statement.Get(), 2, task.Workspace.Value(), error) &&
                   details::BindOptionalText(database, statement.Get(), 3, forkedFrom, error) &&
                   details::BindText(database, statement.Get(), 4, task.Title, error) &&
                   details::BindText(database, statement.Get(), 5, task.Description, error) &&
                   details::BindInt64(database, statement.Get(), 6, static_cast<int64_t>(task.Status), error) &&
                   details::BindInt64(database, statement.Get(), 7, static_cast<int64_t>(task.Priority), error) &&
                   details::BindInt64(database, statement.Get(), 8, static_cast<int64_t>(task.Version), error) &&
                   details::BindInt64(database, statement.Get(), 9, task.ManualHold ? 1 : 0, error) &&
                   details::BindInt64(database, statement.Get(), 10, task.CreatedAtUnixMilliseconds, error) &&
                   details::BindInt64(database, statement.Get(), 11, task.UpdatedAtUnixMilliseconds, error) &&
                   details::StepDone(database, statement.Get(), error);
        }

        bool _insertAcceptanceCriteria(sqlite3* database, const TaskRecord& task, std::string& error)
        {
            constexpr auto sql = R"SQL(
INSERT INTO task_acceptance(task_id, position, text, satisfied)
VALUES(?1, ?2, ?3, ?4);
)SQL";
            for (const auto& criterion : task.AcceptanceCriteria)
            {
                details::SqliteStatement statement{ database, sql, error };
                if (!statement ||
                    !details::BindText(database, statement.Get(), 1, task.Id.Value(), error) ||
                    !details::BindInt64(database, statement.Get(), 2, criterion.Position, error) ||
                    !details::BindText(database, statement.Get(), 3, criterion.Text, error) ||
                    !details::BindInt64(database, statement.Get(), 4, criterion.Satisfied ? 1 : 0, error) ||
                    !details::StepDone(database, statement.Get(), error))
                {
                    return false;
                }
            }
            return true;
        }

        bool _insertExecutionContext(sqlite3* database, const ExecutionContextRecord& context, std::string& error)
        {
            constexpr auto sql = R"SQL(
INSERT INTO execution_context(id, task_id, revision, mode, source_root, runtime_root,
                              repository_common_directory, worktree_path, branch,
                              relative_working_directory, provider, model, instructions,
                              identity_frozen, created_at)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15);
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            return statement &&
                   details::BindText(database, statement.Get(), 1, context.Id.Value(), error) &&
                   details::BindText(database, statement.Get(), 2, context.Task.Value(), error) &&
                   details::BindInt64(database, statement.Get(), 3, static_cast<int64_t>(context.Revision), error) &&
                   details::BindInt64(database, statement.Get(), 4, static_cast<int64_t>(context.Mode), error) &&
                   details::BindText(database, statement.Get(), 5, context.SourceRoot, error) &&
                   details::BindText(database, statement.Get(), 6, context.RuntimeRoot, error) &&
                   details::BindText(database, statement.Get(), 7, context.RepositoryCommonDirectory, error) &&
                   details::BindText(database, statement.Get(), 8, context.WorktreePath, error) &&
                   details::BindText(database, statement.Get(), 9, context.Branch, error) &&
                   details::BindText(database, statement.Get(), 10, context.RelativeWorkingDirectory, error) &&
                   details::BindText(database, statement.Get(), 11, context.Provider, error) &&
                   details::BindText(database, statement.Get(), 12, context.Model, error) &&
                   details::BindText(database, statement.Get(), 13, context.Instructions, error) &&
                   details::BindInt64(database, statement.Get(), 14, context.IdentityFrozen ? 1 : 0, error) &&
                   details::BindInt64(database, statement.Get(), 15, context.CreatedAtUnixMilliseconds, error) &&
                   details::StepDone(database, statement.Get(), error);
        }

        bool _insertTerminal(sqlite3* database, const TaskTerminalRecord& terminal, std::string& error)
        {
            constexpr auto sql = R"SQL(
INSERT INTO task_terminal(id, task_id, state, generation, current_agent_session_id, updated_at)
VALUES(?1, ?2, ?3, ?4, ?5, ?6);
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            const auto agentSession = terminal.CurrentAgentSessionId ?
                                          std::optional<std::string>{ terminal.CurrentAgentSessionId->Value() } :
                                          std::nullopt;
            return statement &&
                   details::BindText(database, statement.Get(), 1, terminal.Id.Value(), error) &&
                   details::BindText(database, statement.Get(), 2, terminal.Task.Value(), error) &&
                   details::BindInt64(database, statement.Get(), 3, static_cast<int64_t>(terminal.State), error) &&
                   details::BindInt64(database, statement.Get(), 4, static_cast<int64_t>(terminal.Generation), error) &&
                   details::BindOptionalText(database, statement.Get(), 5, agentSession, error) &&
                   details::BindInt64(database, statement.Get(), 6, terminal.UpdatedAtUnixMilliseconds, error) &&
                   details::StepDone(database, statement.Get(), error);
        }

        bool _updateTaskStatus(
            sqlite3* database,
            const ChangeTaskStatusCommand& command,
            const uint64_t committedVersion,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE task SET status=?1, version=?2, updated_at=?3
WHERE id=?4 AND version=?5;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement ||
                !details::BindInt64(database, statement.Get(), 1, static_cast<int64_t>(command.RequestedStatus), error) ||
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
                error = "Task version changed during status transition";
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

    TaskStoreCommandResult SqliteTaskStore::CreateTask(const CreateTaskCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Task.Id, CreateTaskCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateCreateTaskCommand(command, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, std::move(error));
        }

        std::optional<details::StoredTaskState> existing;
        if (!details::LoadTaskState(_impl->Database, command.Task.Id, existing, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (existing)
        {
            return details::CommandFailure(TaskStoreCommandStatus::VersionConflict, "Task already exists");
        }
        if (command.Task.ForkedFromTaskId)
        {
            std::optional<details::StoredTaskState> parent;
            if (!details::LoadTaskState(_impl->Database, command.Task.ForkedFromTaskId.value(), parent, error))
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
            }
            if (!parent)
            {
                return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, "Fork source Task does not exist");
            }
        }

        constexpr uint64_t eventSequence{ 1 };
        if (!_insertTask(_impl->Database, command.Task, error) ||
            !_insertAcceptanceCriteria(_impl->Database, command.Task, error) ||
            !_insertExecutionContext(_impl->Database, command.ExecutionContext, error) ||
            !_insertTerminal(_impl->Database, command.Terminal, error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Task.Id,
                std::nullopt,
                eventSequence,
                std::nullopt,
                "TaskCreated",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task.Id,
                CreateTaskCommandName,
                command.Task.Version,
                eventSequence,
                error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }

        return _success(command.Metadata, command.Task.Version, eventSequence);
    }

    TaskStoreCommandResult SqliteTaskStore::ChangeTaskStatus(const ChangeTaskStatusCommand& command)
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
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Task, ChangeTaskStatusCommandName))
        {
            return replay.value();
        }
        if (!details::ValidateChangeTaskStatusCommand(command, error))
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

        TaskStatusTransitionContext context;
        if (!details::HasActiveRun(_impl->Database, command.Task, context.HasActiveRun, error) ||
            !details::HasUnclaimedMessages(_impl->Database, command.Task, context.HasUnclaimedMessages, error) ||
            !details::HasPendingAutomaticRetry(_impl->Database, command.Task, context.HasPendingAutomaticRetry, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }

        const auto decision = EvaluateTaskStatusTransition(task->Status, command.RequestedStatus, command.Authority, context);
        if (!decision.Allowed)
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "Task status transition was rejected", decision.Error);
        }

        uint64_t eventSequence{};
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Task, eventSequence, error) ||
            !_updateTaskStatus(_impl->Database, command, committedVersion, error) ||
            !details::AppendEvent(
                _impl->Database,
                command.Metadata,
                command.Task,
                std::nullopt,
                eventSequence,
                std::nullopt,
                "TaskStatusChanged",
                error) ||
            !details::InsertReceipt(
                _impl->Database,
                command.Metadata,
                command.Task,
                ChangeTaskStatusCommandName,
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
