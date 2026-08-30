// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreRowSqlite.h"
#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        template<typename TRecord, typename TReader>
        bool _loadBoundedRows(
            sqlite3* database,
            const char* sql,
            const size_t limit,
            std::vector<TRecord>& rows,
            bool& hasMore,
            TReader&& reader,
            std::string& error)
        {
            details::SqliteStatement statement{ database, sql, error };
            if (!statement ||
                !details::BindInt64(database, statement.Get(), 1, static_cast<int64_t>(limit + 1), error))
            {
                return false;
            }
            for (auto step = sqlite3_step(statement.Get()); step != SQLITE_DONE; step = sqlite3_step(statement.Get()))
            {
                if (step != SQLITE_ROW)
                {
                    error = details::SqliteError(database, step, "load recovery rows");
                    return false;
                }
                rows.emplace_back(reader(statement.Get()));
            }
            hasMore = rows.size() > limit;
            if (hasMore)
            {
                rows.pop_back();
            }
            return true;
        }
    }

    TaskRecoveryQueryResult SqliteTaskStore::LoadRecoverySnapshot(const size_t limit) const
    {
        TaskRecoveryQueryResult result;
        if (!IsReady())
        {
            result.Error = LastError();
            return result;
        }
        if (limit == 0)
        {
            result.Error = "Recovery query limit must be greater than zero";
            return result;
        }

        std::scoped_lock lock{ _impl->DatabaseMutex };
        const auto boundedLimit = std::min(limit, MaximumTaskRecoveryQueryCount);
        std::string error;

        constexpr auto activeRunsSql = R"SQL(
SELECT id, task_id, execution_context_id, agent_session_id, retry_of_run_id, trigger,
       state, sequence, created_at, started_at, finished_at, result, reason_code
FROM task_run
WHERE state IN (0,1,2,3,4)
ORDER BY created_at, task_id, sequence
LIMIT ?1;
)SQL";
        if (!_loadBoundedRows(
                _impl->Database,
                activeRunsSql,
                boundedLimit,
                result.Snapshot.ActiveRuns,
                result.Snapshot.HasMoreActiveRuns,
                details::ReadTaskRunRow,
                error))
        {
            result.Error = std::move(error);
            return result;
        }

        constexpr auto pendingIntentsSql = R"SQL(
SELECT id, task_id, kind, stage, revision, deadline_at, detail_json, reason_code,
       created_at, updated_at
FROM task_intent
WHERE stage<>8
ORDER BY updated_at, task_id
LIMIT ?1;
)SQL";
        if (!_loadBoundedRows(
                _impl->Database,
                pendingIntentsSql,
                boundedLimit,
                result.Snapshot.PendingIntents,
                result.Snapshot.HasMorePendingIntents,
                details::ReadTaskIntentRow,
                error))
        {
            result.Error = std::move(error);
            return result;
        }

        constexpr auto terminalsSql = R"SQL(
SELECT id, task_id, state, generation, current_agent_session_id, updated_at
FROM task_terminal
WHERE state IN (1,2,3)
ORDER BY updated_at, task_id
LIMIT ?1;
)SQL";
        if (!_loadBoundedRows(
                _impl->Database,
                terminalsSql,
                boundedLimit,
                result.Snapshot.RecoverableTerminals,
                result.Snapshot.HasMoreRecoverableTerminals,
                details::ReadTaskTerminalRow,
                error))
        {
            result.Error = std::move(error);
            return result;
        }

        constexpr auto messagesSql = R"SQL(
SELECT id, task_id, author, state, revision, content, claimed_by_run_id, created_at, updated_at
FROM task_message
WHERE state IN (0,1)
ORDER BY created_at, task_id, id
LIMIT ?1;
)SQL";
        if (!_loadBoundedRows(
                _impl->Database,
                messagesSql,
                boundedLimit,
                result.Snapshot.UnclaimedMessages,
                result.Snapshot.HasMoreUnclaimedMessages,
                details::ReadTaskMessageRow,
                error))
        {
            result.Error = std::move(error);
            return result;
        }

        constexpr auto tombstonesSql = R"SQL(
SELECT task_id, deleted_at, expires_at, replay_digest, command_id, expected_version,
       committed_version, last_event_sequence
FROM delete_tombstone
ORDER BY expires_at, task_id
LIMIT ?1;
)SQL";
        if (!_loadBoundedRows(
                _impl->Database,
                tombstonesSql,
                boundedLimit,
                result.Snapshot.Tombstones,
                result.Snapshot.HasMoreTombstones,
                details::ReadDeleteTombstoneRow,
                error))
        {
            result.Error = std::move(error);
            return result;
        }

        result.Succeeded = true;
        return result;
    }
}
