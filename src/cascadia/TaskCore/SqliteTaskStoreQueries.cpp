// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreCommandSqlite.h"
#include "TaskStoreRowSqlite.h"
#include "TaskStoreSqlite.h"

#include <limits>
#include <unordered_map>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr auto TaskColumns = R"SQL(
id, workspace_id, forked_from_task_id, title, description, status, priority,
version, manual_hold, created_at, updated_at
)SQL";

        template<typename TRecord, typename TReader>
        bool _loadTaskRows(
            sqlite3* database,
            const char* sql,
            const TaskId& task,
            std::vector<TRecord>& rows,
            TReader&& reader,
            std::string& error)
        {
            details::SqliteStatement statement{ database, sql, error };
            if (!statement || !details::BindText(database, statement.Get(), 1, task.Value(), error))
            {
                return false;
            }
            for (auto step = sqlite3_step(statement.Get()); step != SQLITE_DONE; step = sqlite3_step(statement.Get()))
            {
                if (step != SQLITE_ROW)
                {
                    error = details::SqliteError(database, step, "read Task aggregate rows");
                    return false;
                }
                rows.emplace_back(reader(statement.Get()));
            }
            return true;
        }

        bool _loadAcceptanceCriteria(
            sqlite3* database,
            const TaskId& task,
            std::vector<AcceptanceCriterionRecord>& criteria,
            std::string& error)
        {
            return _loadTaskRows(
                database,
                "SELECT position, text, satisfied FROM task_acceptance WHERE task_id=?1 ORDER BY position;",
                task,
                criteria,
                [](sqlite3_stmt* statement) { return details::ReadAcceptanceCriterionRow(statement); },
                error);
        }

        bool _loadExecutionContexts(
            sqlite3* database,
            const TaskId& task,
            std::vector<ExecutionContextRecord>& contexts,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
SELECT id, task_id, revision, mode, source_root, runtime_root, repository_common_directory,
       worktree_path, branch, relative_working_directory, provider, model, instructions,
       identity_frozen, created_at
FROM execution_context WHERE task_id=?1 ORDER BY revision;
)SQL";
            return _loadTaskRows(database, sql, task, contexts, details::ReadExecutionContextRow, error);
        }

        bool _loadAgentSessions(
            sqlite3* database,
            const TaskId& task,
            std::vector<AgentSessionRecord>& sessions,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
SELECT id, task_id, provider, provider_session_id, state, is_current, created_at, updated_at
FROM agent_session WHERE task_id=?1 ORDER BY created_at, id;
)SQL";
            return _loadTaskRows(database, sql, task, sessions, details::ReadAgentSessionRow, error);
        }

        bool _loadRuns(
            sqlite3* database,
            const TaskId& task,
            std::vector<TaskRunRecord>& runs,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
SELECT id, task_id, execution_context_id, agent_session_id, retry_of_run_id, trigger,
       state, sequence, created_at, started_at, finished_at, result, reason_code
FROM task_run WHERE task_id=?1 ORDER BY sequence;
)SQL";
            return _loadTaskRows(database, sql, task, runs, details::ReadTaskRunRow, error);
        }

        bool _loadMessages(
            sqlite3* database,
            const TaskId& task,
            std::vector<TaskMessageRecord>& messages,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
SELECT id, task_id, state, revision, content, claimed_by_run_id, created_at
FROM task_message WHERE task_id=?1 ORDER BY created_at, id;
)SQL";
            return _loadTaskRows(database, sql, task, messages, details::ReadTaskMessageRow, error);
        }

        bool _loadTerminal(
            sqlite3* database,
            const TaskId& task,
            std::optional<TaskTerminalRecord>& terminal,
            std::string& error)
        {
            constexpr auto sql = R"SQL(
SELECT id, task_id, state, generation, current_agent_session_id, updated_at
FROM task_terminal WHERE task_id=?1;
)SQL";
            details::SqliteStatement statement{ database, sql, error };
            if (!statement || !details::BindText(database, statement.Get(), 1, task.Value(), error))
            {
                return false;
            }
            const auto step = sqlite3_step(statement.Get());
            if (step == SQLITE_DONE)
            {
                terminal.reset();
                return true;
            }
            if (step != SQLITE_ROW)
            {
                error = details::SqliteError(database, step, "read TaskTerminal");
                return false;
            }
            terminal = details::ReadTaskTerminalRow(statement.Get());
            return true;
        }

        bool _loadLastEventSequence(
            sqlite3* database,
            const TaskId& task,
            uint64_t& sequence,
            std::string& error)
        {
            details::SqliteStatement statement{
                database,
                "SELECT COALESCE(MAX(aggregate_sequence),0) FROM task_event WHERE task_id=?1;",
                error
            };
            if (!statement || !details::BindText(database, statement.Get(), 1, task.Value(), error))
            {
                return false;
            }
            const auto step = sqlite3_step(statement.Get());
            if (step != SQLITE_ROW)
            {
                error = details::SqliteError(database, step, "read last Task event sequence");
                return false;
            }
            sequence = static_cast<uint64_t>(sqlite3_column_int64(statement.Get(), 0));
            return true;
        }
    }

    std::optional<TaskAggregateSnapshot> SqliteTaskStore::LoadTask(const TaskId& task) const
    {
        if (!IsReady() || !IsCanonicalUuid(task.Value()))
        {
            return std::nullopt;
        }

        std::scoped_lock lock{ _impl->DatabaseMutex };
        std::string error;
        const std::string sql = "SELECT " + std::string{ TaskColumns } + " FROM task WHERE id=?1;";
        details::SqliteStatement statement{ _impl->Database, sql.c_str(), error };
        if (!statement || !details::BindText(_impl->Database, statement.Get(), 1, task.Value(), error))
        {
            return std::nullopt;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step == SQLITE_DONE)
        {
            return std::nullopt;
        }
        if (step != SQLITE_ROW)
        {
            return std::nullopt;
        }

        TaskAggregateSnapshot snapshot;
        snapshot.Task = details::ReadTaskRow(statement.Get());
        std::optional<TaskTerminalRecord> terminal;
        if (!_loadAcceptanceCriteria(_impl->Database, task, snapshot.Task.AcceptanceCriteria, error) ||
            !_loadExecutionContexts(_impl->Database, task, snapshot.ExecutionContexts, error) ||
            !_loadTerminal(_impl->Database, task, terminal, error) ||
            !terminal ||
            !details::LoadTaskIntent(_impl->Database, task, snapshot.Intent, error) ||
            !_loadAgentSessions(_impl->Database, task, snapshot.AgentSessions, error) ||
            !_loadRuns(_impl->Database, task, snapshot.Runs, error) ||
            !_loadMessages(_impl->Database, task, snapshot.Messages, error) ||
            !_loadLastEventSequence(_impl->Database, task, snapshot.LastEventSequence, error))
        {
            return std::nullopt;
        }
        snapshot.Terminal = std::move(terminal.value());
        return snapshot;
    }

    std::vector<TaskRecord> SqliteTaskStore::ListTasks(const std::optional<WorkspaceId>& workspace) const
    {
        if (!IsReady() || (workspace && !IsCanonicalUuid(workspace->Value())))
        {
            return {};
        }

        std::scoped_lock lock{ _impl->DatabaseMutex };
        std::string error;
        const std::string sql = workspace ?
                                    "SELECT " + std::string{ TaskColumns } + " FROM task WHERE workspace_id=?1 ORDER BY updated_at DESC, id;" :
                                    "SELECT " + std::string{ TaskColumns } + " FROM task ORDER BY updated_at DESC, id;";
        details::SqliteStatement statement{ _impl->Database, sql.c_str(), error };
        if (!statement ||
            (workspace && !details::BindText(_impl->Database, statement.Get(), 1, workspace->Value(), error)))
        {
            return {};
        }

        std::vector<TaskRecord> tasks;
        for (auto step = sqlite3_step(statement.Get()); step != SQLITE_DONE; step = sqlite3_step(statement.Get()))
        {
            if (step != SQLITE_ROW)
            {
                return {};
            }
            tasks.emplace_back(details::ReadTaskRow(statement.Get()));
        }
        if (tasks.empty())
        {
            return tasks;
        }

        std::unordered_map<std::string, size_t> taskIndex;
        taskIndex.reserve(tasks.size());
        for (size_t index = 0; index < tasks.size(); ++index)
        {
            taskIndex.emplace(tasks[index].Id.Value(), index);
        }

        constexpr auto allCriteriaSql = R"SQL(
SELECT a.task_id, a.position, a.text, a.satisfied
FROM task_acceptance AS a
JOIN task AS t ON t.id=a.task_id
ORDER BY a.task_id, a.position;
)SQL";
        constexpr auto workspaceCriteriaSql = R"SQL(
SELECT a.task_id, a.position, a.text, a.satisfied
FROM task_acceptance AS a
JOIN task AS t ON t.id=a.task_id
WHERE t.workspace_id=?1
ORDER BY a.task_id, a.position;
)SQL";
        details::SqliteStatement criteriaStatement{
            _impl->Database,
            workspace ? workspaceCriteriaSql : allCriteriaSql,
            error
        };
        if (!criteriaStatement ||
            (workspace && !details::BindText(_impl->Database, criteriaStatement.Get(), 1, workspace->Value(), error)))
        {
            return {};
        }
        for (auto step = sqlite3_step(criteriaStatement.Get()); step != SQLITE_DONE; step = sqlite3_step(criteriaStatement.Get()))
        {
            if (step != SQLITE_ROW)
            {
                return {};
            }
            const auto match = taskIndex.find(details::ColumnText(criteriaStatement.Get(), 0));
            if (match != taskIndex.end())
            {
                tasks[match->second].AcceptanceCriteria.emplace_back(
                    details::ReadAcceptanceCriterionRow(criteriaStatement.Get(), 1));
            }
        }
        return tasks;
    }

    std::vector<TaskEventRecord> SqliteTaskStore::ListEvents(
        const TaskId& task,
        const uint64_t afterSequence,
        const size_t limit) const
    {
        if (!IsReady() || !IsCanonicalUuid(task.Value()) || limit == 0 ||
            afterSequence > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return {};
        }

        std::scoped_lock lock{ _impl->DatabaseMutex };
        std::string error;
        constexpr auto sql = R"SQL(
SELECT id, task_id, run_id, aggregate_sequence, run_sequence, type, payload_json, created_at
FROM task_event
WHERE task_id=?1 AND aggregate_sequence>?2
ORDER BY aggregate_sequence
LIMIT ?3;
)SQL";
        details::SqliteStatement statement{ _impl->Database, sql, error };
        const auto boundedLimit = std::min(limit, MaximumTaskEventQueryCount);
        if (!statement ||
            !details::BindText(_impl->Database, statement.Get(), 1, task.Value(), error) ||
            !details::BindInt64(_impl->Database, statement.Get(), 2, static_cast<int64_t>(afterSequence), error) ||
            !details::BindInt64(_impl->Database, statement.Get(), 3, static_cast<int64_t>(boundedLimit), error))
        {
            return {};
        }

        std::vector<TaskEventRecord> events;
        events.reserve(boundedLimit);
        for (auto step = sqlite3_step(statement.Get()); step != SQLITE_DONE; step = sqlite3_step(statement.Get()))
        {
            if (step != SQLITE_ROW)
            {
                return {};
            }
            events.emplace_back(details::ReadTaskEventRow(statement.Get()));
        }
        return events;
    }
}
