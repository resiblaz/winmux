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
        constexpr std::string_view RecordTaskAgentSessionCommandName{ "RecordTaskAgentSession" };
        constexpr std::string_view AppendTaskProviderEventCommandName{ "AppendTaskProviderEvent" };

        struct StoredAgentSession
        {
            TaskId Task;
            std::string Provider;
            std::string ProviderSessionId;
            AgentSessionState State{ AgentSessionState::Available };
            bool IsCurrent{};
            int64_t CreatedAtUnixMilliseconds{};
            int64_t UpdatedAtUnixMilliseconds{};
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

        bool _loadAgentSession(
            sqlite3* database,
            const AgentSessionId& id,
            std::optional<StoredAgentSession>& result,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
SELECT task_id, provider, provider_session_id, state, is_current, created_at, updated_at
FROM agent_session WHERE id=?1;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement || !details::BindText(database, statement.Get(), 1, id.Value(), error)) return false;
            const auto step = sqlite3_step(statement.Get());
            if (step == SQLITE_DONE)
            {
                result.reset();
                return true;
            }
            if (step != SQLITE_ROW)
            {
                error = details::SqliteError(database, step, "read AgentSession");
                return false;
            }
            result = StoredAgentSession{
                TaskId{ details::ColumnText(statement.Get(), 0) },
                details::ColumnText(statement.Get(), 1),
                details::ColumnText(statement.Get(), 2),
                static_cast<AgentSessionState>(sqlite3_column_int(statement.Get(), 3)),
                sqlite3_column_int(statement.Get(), 4) != 0,
                sqlite3_column_int64(statement.Get(), 5),
                sqlite3_column_int64(statement.Get(), 6),
            };
            return true;
        }

        bool _providerSessionAvailable(
            sqlite3* database,
            const AgentSessionRecord& session,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
SELECT id, task_id FROM agent_session
WHERE provider=?1 AND provider_session_id=?2 LIMIT 1;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement ||
                !details::BindText(database, statement.Get(), 1, session.Provider, error) ||
                !details::BindText(database, statement.Get(), 2, session.ProviderSessionId, error))
            {
                return false;
            }
            const auto step = sqlite3_step(statement.Get());
            if (step == SQLITE_DONE) return true;
            if (step != SQLITE_ROW)
            {
                error = details::SqliteError(database, step, "read provider AgentSession binding");
                return false;
            }
            if (details::ColumnText(statement.Get(), 0) != session.Id.Value() ||
                details::ColumnText(statement.Get(), 1) != session.Task.Value())
            {
                error = "Provider AgentSession identity is already bound";
                return false;
            }
            return true;
        }

        bool _sessionTransitionAllowed(
            const AgentSessionState current,
            const AgentSessionState requested) noexcept
        {
            if (current == requested) return true;
            switch (current)
            {
            case AgentSessionState::Available:
                return requested == AgentSessionState::Suspended ||
                       requested == AgentSessionState::Poisoned ||
                       requested == AgentSessionState::Closed;
            case AgentSessionState::Suspended:
                return requested == AgentSessionState::Available ||
                       requested == AgentSessionState::Poisoned ||
                       requested == AgentSessionState::Closed;
            case AgentSessionState::Poisoned:
                return requested == AgentSessionState::Closed;
            default:
                return false;
            }
        }

        bool _clearOtherCurrentSessions(
            sqlite3* database,
            const AgentSessionRecord& session,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE agent_session SET is_current=0
WHERE task_id=?1 AND id<>?2 AND is_current=1;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            return statement &&
                   details::BindText(database, statement.Get(), 1, session.Task.Value(), error) &&
                   details::BindText(database, statement.Get(), 2, session.Id.Value(), error) &&
                   details::StepDone(database, statement.Get(), error);
        }

        bool _insertAgentSession(
            sqlite3* database,
            const AgentSessionRecord& session,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
INSERT INTO agent_session(
    id, task_id, provider, provider_session_id, state, is_current, created_at, updated_at)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            return statement &&
                   details::BindText(database, statement.Get(), 1, session.Id.Value(), error) &&
                   details::BindText(database, statement.Get(), 2, session.Task.Value(), error) &&
                   details::BindText(database, statement.Get(), 3, session.Provider, error) &&
                   details::BindText(database, statement.Get(), 4, session.ProviderSessionId, error) &&
                   details::BindInt64(database, statement.Get(), 5, static_cast<int64_t>(session.State), error) &&
                   details::BindInt64(database, statement.Get(), 6, session.IsCurrent ? 1 : 0, error) &&
                   details::BindInt64(database, statement.Get(), 7, session.CreatedAtUnixMilliseconds, error) &&
                   details::BindInt64(database, statement.Get(), 8, session.UpdatedAtUnixMilliseconds, error) &&
                   details::StepDone(database, statement.Get(), error);
        }

        bool _updateAgentSession(
            sqlite3* database,
            const AgentSessionRecord& session,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
UPDATE agent_session SET state=?1, is_current=?2, updated_at=?3
WHERE id=?4 AND task_id=?5;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            return statement &&
                   details::BindInt64(database, statement.Get(), 1, static_cast<int64_t>(session.State), error) &&
                   details::BindInt64(database, statement.Get(), 2, session.IsCurrent ? 1 : 0, error) &&
                   details::BindInt64(database, statement.Get(), 3, session.UpdatedAtUnixMilliseconds, error) &&
                   details::BindText(database, statement.Get(), 4, session.Id.Value(), error) &&
                   details::BindText(database, statement.Get(), 5, session.Task.Value(), error) &&
                   details::StepDone(database, statement.Get(), error) && sqlite3_changes(database) == 1;
        }

        bool _updateTerminalSession(
            sqlite3* database,
            const AgentSessionRecord& session,
            const bool wasCurrent,
            std::string& error)
        {
            if (!session.IsCurrent && !wasCurrent) return true;
            constexpr auto sql = R"SQL(
UPDATE task_terminal
SET current_agent_session_id=?1, updated_at=?2
WHERE task_id=?3;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            const auto current = session.IsCurrent ?
                                     std::optional<std::string>{ session.Id.Value() } :
                                     std::nullopt;
            return statement &&
                   details::BindOptionalText(database, statement.Get(), 1, current, error) &&
                   details::BindInt64(database, statement.Get(), 2, session.UpdatedAtUnixMilliseconds, error) &&
                   details::BindText(database, statement.Get(), 3, session.Task.Value(), error) &&
                   details::StepDone(database, statement.Get(), error) && sqlite3_changes(database) == 1;
        }
    }

    TaskStoreCommandResult SqliteTaskStore::RecordTaskAgentSession(
        const RecordTaskAgentSessionCommand& command)
    {
        if (!IsReady()) return details::CommandFailure(TaskStoreCommandStatus::NotReady, LastError());
        std::string error;
        if (!details::ValidateRecordTaskAgentSessionCommand(command, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, std::move(error));
        }
        std::scoped_lock lock{ _impl->DatabaseMutex };
        details::SqliteTransaction transaction{ _impl->Database, error };
        if (!transaction) return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));

        std::optional<details::StoredReceipt> receipt;
        if (!details::LookupReceipt(_impl->Database, command.Metadata.Command, receipt, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (const auto replay = details::EvaluateReceiptReplay(
                receipt, command.Metadata, command.Session.Task, RecordTaskAgentSessionCommandName))
        {
            return replay.value();
        }
        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(_impl->Database, command.Session.Task, task, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!task) return details::MissingTaskFailure(_impl->Database, command.Session.Task, error);
        if (task->Version != command.Metadata.ExpectedTaskVersion)
        {
            return details::CommandFailure(TaskStoreCommandStatus::VersionConflict, "Task version does not match expected version");
        }
        if (command.Metadata.TimestampUnixMilliseconds < task->UpdatedAtUnixMilliseconds ||
            task->Version == static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "AgentSession command time or Task version is invalid");
        }

        std::optional<StoredAgentSession> existing;
        if (!_loadAgentSession(_impl->Database, command.Session.Id, existing, error) ||
            !_providerSessionAvailable(_impl->Database, command.Session, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (existing &&
            (existing->Task != command.Session.Task || existing->Provider != command.Session.Provider ||
             existing->ProviderSessionId != command.Session.ProviderSessionId ||
             existing->CreatedAtUnixMilliseconds != command.Session.CreatedAtUnixMilliseconds))
        {
            return details::CommandFailure(TaskStoreCommandStatus::IdempotencyConflict, "AgentSession immutable identity changed");
        }
        if (existing &&
            (!_sessionTransitionAllowed(existing->State, command.Session.State) ||
             command.Session.UpdatedAtUnixMilliseconds < existing->UpdatedAtUnixMilliseconds))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidTransition, "AgentSession state transition was rejected");
        }

        uint64_t eventSequence{};
        const auto committedVersion = task->Version + 1;
        const auto inserted = !existing;
        if ((command.Session.IsCurrent && !_clearOtherCurrentSessions(_impl->Database, command.Session, error)) ||
            (inserted ? !_insertAgentSession(_impl->Database, command.Session, error) :
                        !_updateAgentSession(_impl->Database, command.Session, error)) ||
            !_updateTerminalSession(_impl->Database, command.Session, existing && existing->IsCurrent, error) ||
            !details::NextAggregateSequence(_impl->Database, command.Session.Task, eventSequence, error) ||
            !details::UpdateTaskVersion(
                _impl->Database, command.Session.Task, task->Version, committedVersion,
                command.Metadata.TimestampUnixMilliseconds, error) ||
            !details::AppendEvent(
                _impl->Database, command.Metadata, command.Session.Task, std::nullopt,
                eventSequence, std::nullopt,
                inserted ? "AgentSessionRecorded" : "AgentSessionStateChanged", error) ||
            !details::InsertReceipt(
                _impl->Database, command.Metadata, command.Session.Task,
                RecordTaskAgentSessionCommandName, committedVersion, eventSequence, error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        return _success(command.Metadata, committedVersion, eventSequence);
    }

    TaskStoreCommandResult SqliteTaskStore::AppendTaskProviderEvent(
        const AppendTaskProviderEventCommand& command)
    {
        if (!IsReady()) return details::CommandFailure(TaskStoreCommandStatus::NotReady, LastError());
        std::string error;
        if (!details::ValidateAppendTaskProviderEventCommand(command, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, std::move(error));
        }
        std::scoped_lock lock{ _impl->DatabaseMutex };
        details::SqliteTransaction transaction{ _impl->Database, error };
        if (!transaction) return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));

        std::optional<details::StoredReceipt> receipt;
        if (!details::LookupReceipt(_impl->Database, command.Metadata.Command, receipt, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (const auto replay = details::EvaluateReceiptReplay(
                receipt, command.Metadata, command.Task, AppendTaskProviderEventCommandName))
        {
            return replay.value();
        }
        std::optional<details::StoredTaskState> task;
        if (!details::LoadTaskState(_impl->Database, command.Task, task, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (!task) return details::MissingTaskFailure(_impl->Database, command.Task, error);
        if (task->Version != command.Metadata.ExpectedTaskVersion)
        {
            return details::CommandFailure(TaskStoreCommandStatus::VersionConflict, "Task version does not match expected version");
        }
        if (command.Metadata.TimestampUnixMilliseconds < task->UpdatedAtUnixMilliseconds ||
            task->Version == static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
        {
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Provider event time or Task version is invalid");
        }
        if (command.Run)
        {
            std::optional<details::StoredTaskRunState> run;
            if (!details::LoadTaskRunState(_impl->Database, *command.Run, run, error))
            {
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
            }
            if (!run || run->Task != command.Task)
            {
                return details::CommandFailure(TaskStoreCommandStatus::NotFound, "Provider event TaskRun was not found");
            }
            if (command.Metadata.TimestampUnixMilliseconds < run->CreatedAtUnixMilliseconds)
            {
                return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Provider event precedes its TaskRun");
            }
        }

        uint64_t eventSequence{};
        std::optional<uint64_t> runSequence;
        uint64_t nextRunSequence{};
        if (command.Run && !details::NextRunEventSequence(_impl->Database, *command.Run, nextRunSequence, error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        if (command.Run) runSequence = nextRunSequence;
        const auto committedVersion = task->Version + 1;
        if (!details::NextAggregateSequence(_impl->Database, command.Task, eventSequence, error) ||
            !details::UpdateTaskVersion(
                _impl->Database, command.Task, task->Version, committedVersion,
                command.Metadata.TimestampUnixMilliseconds, error) ||
            !details::AppendEvent(
                _impl->Database, command.Metadata, command.Task, command.Run,
                eventSequence, runSequence, command.EventType, error) ||
            !details::InsertReceipt(
                _impl->Database, command.Metadata, command.Task,
                AppendTaskProviderEventCommandName, committedVersion, eventSequence, error) ||
            !transaction.Commit(error))
        {
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, std::move(error));
        }
        return _success(command.Metadata, committedVersion, eventSequence);
    }
}
