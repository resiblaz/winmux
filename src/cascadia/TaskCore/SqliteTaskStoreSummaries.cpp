// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "SqliteTaskStoreInternal.h"
#include "TaskStoreSqlite.h"
#include <limits>
#include <wil/resource.h>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        constexpr std::string_view filter = R"SQL(
WHERE (?1='' OR t.workspace_id=?1) AND (?2<0 OR t.status=?2)
 AND (?3<0 OR (CASE t.status WHEN 4 THEN 2 ELSE t.status END)=?3)
 AND (?4=0 OR t.status NOT IN(5,6))
 AND (?5='' OR instr(lower(t.id||' '||t.title||' '||p.name),lower(?5))>0)
)SQL";
        bool bind(sqlite3* db, sqlite3_stmt* statement, const TaskSummaryQuery& q, std::string& error)
        {
            return details::BindText(db, statement, 1, q.Workspace ? q.Workspace->Value() : "", error) &&
                   details::BindInt64(db, statement, 2, q.Status ? static_cast<int64_t>(*q.Status) : -1, error) &&
                   details::BindInt64(db, statement, 3, q.Bucket ? static_cast<int64_t>(TaskBoardBucket(*q.Bucket)) : -1, error) &&
                   details::BindInt64(db, statement, 4, q.OpenOnly, error) && details::BindText(db, statement, 5, q.Search, error);
        }
    }

    TaskSummaryQueryResult SqliteTaskStore::QueryTaskSummaries(const TaskSummaryQuery& query) const
    {
        TaskSummaryQueryResult result;
        if (!IsReady()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = LastError(); return result; }
        if (!query.Limit || query.Limit > MaximumTaskSummaryPageSize || query.Sort > TaskSummarySort::BoardOrder ||
            (query.Workspace && !IsCanonicalUuid(query.Workspace->Value())) ||
            (query.Status && *query.Status > TaskStatus::Cancelled) || (query.Bucket && *query.Bucket > TaskStatus::Cancelled) ||
            (!query.AfterId.empty() && !IsCanonicalUuid(query.AfterId)) || query.Search.size() > 1024 || query.Search.find('\0') != std::string::npos ||
            (query.Sort == TaskSummarySort::BoardOrder && (!query.Workspace || !query.Bucket)))
        { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = "task_summary_invalid_query"; return result; }
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        auto db = _impl->Database;
        if (!details::ExecuteSql(db, "BEGIN;", result.Error)) return result;
        const auto rollback = wil::scope_exit([&] { std::string ignored; static_cast<void>(details::ExecuteSql(db, "ROLLBACK;", ignored)); });
        const auto generation = details::QueryInt64(db, "SELECT coalesce(max(sequence),0) FROM project_event;", result.Error);
        if (!generation) return result;
        result.Generation = *generation;
        if (query.Workspace && query.Bucket)
        {
            details::SqliteStatement revision{ db, "SELECT revision FROM project_board_column WHERE workspace_id=?1 AND bucket=?2;", result.Error };
            if (!revision || !details::BindText(db, revision.Get(), 1, query.Workspace->Value(), result.Error) ||
                !details::BindInt64(db, revision.Get(), 2, static_cast<int64_t>(TaskBoardBucket(*query.Bucket)), result.Error)) return result;
            const auto step = sqlite3_step(revision.Get());
            if (step == SQLITE_ROW) result.ColumnRevision = sqlite3_column_int64(revision.Get(), 0);
            else if (step != SQLITE_DONE) { result.Error = details::SqliteError(db, step, "read board revision"); return result; }
            if (query.ExpectedColumnRevision && query.ExpectedColumnRevision != result.ColumnRevision)
            { result.Status = TaskStoreCommandStatus::VersionConflict; result.Error = "stale_cursor"; return result; }
        }
        const auto countSql = "SELECT t.status,count(*) FROM task t JOIN project p ON p.workspace_id=t.workspace_id " + std::string{ filter } + " GROUP BY t.status;";
        details::SqliteStatement counts{ db, countSql.c_str(), result.Error };
        if (!counts || !bind(db, counts.Get(), query, result.Error)) return result;
        int step;
        while ((step = sqlite3_step(counts.Get())) == SQLITE_ROW)
        {
            const auto state = sqlite3_column_int(counts.Get(), 0);
            if (state < 0 || state > 6) { result.Error = "Invalid stored summary state"; return result; }
            const auto count = static_cast<uint64_t>(sqlite3_column_int64(counts.Get(), 1));
            result.Counts[static_cast<size_t>(state)] = count;
            result.Total += count;
        }
        if (step != SQLITE_DONE) { result.Error = details::SqliteError(db, step, "count task summaries"); return result; }
        std::string sql = R"SQL(
SELECT t.id,t.workspace_id,p.name,t.title,t.status,t.priority,t.version,t.manual_hold,t.updated_at,b.rank,
 c.id,c.source_agent_id,c.source_agent_revision,c.agent_name_snapshot,c.model,c.mode,
 r.id,r.state,r.sequence,
 (SELECT coalesce(max(aggregate_sequence),0) FROM task_event WHERE task_id=t.id),
 EXISTS(SELECT 1 FROM task_message WHERE task_id=t.id AND state IN(0,1)),
 (EXISTS(SELECT 1 FROM task_intent WHERE task_id=t.id AND stage NOT IN(7,8)) OR EXISTS(SELECT 1 FROM task_board_operation WHERE task_id=t.id AND stage=0)),p.archived_at,
 (SELECT revision FROM project_board_column WHERE workspace_id=t.workspace_id AND bucket=(CASE t.status WHEN 4 THEN 2 ELSE t.status END))
FROM task t JOIN project p ON p.workspace_id=t.workspace_id JOIN task_board_position b ON b.task_id=t.id
LEFT JOIN task_run r ON r.id=(SELECT id FROM task_run WHERE task_id=t.id ORDER BY (state BETWEEN 0 AND 4) DESC,sequence DESC LIMIT 1)
LEFT JOIN execution_context c ON c.id=coalesce(
 (SELECT execution_context_id FROM task_run WHERE task_id=t.id AND state BETWEEN 0 AND 4 LIMIT 1),
 (SELECT id FROM execution_context WHERE task_id=t.id ORDER BY revision DESC LIMIT 1))
)SQL";
        sql += filter;
        if (query.Sort == TaskSummarySort::BoardOrder)
            sql += " AND (?6='' OR b.rank>?7 OR (b.rank=?7 AND t.id>?6)) ORDER BY b.rank,t.id LIMIT ?8;";
        else
            sql += " AND (?6='' OR t.updated_at<?7 OR (t.updated_at=?7 AND t.id>?6)) ORDER BY t.updated_at DESC,t.id LIMIT ?8;";
        details::SqliteStatement page{ db, sql.c_str(), result.Error };
        if (!page || !bind(db, page.Get(), query, result.Error) || !details::BindText(db, page.Get(), 6, query.AfterId, result.Error) ||
            !details::BindInt64(db, page.Get(), 7, query.AfterValue, result.Error) ||
            !details::BindInt64(db, page.Get(), 8, static_cast<int64_t>(query.Limit + 1), result.Error)) return result;
        while ((step = sqlite3_step(page.Get())) == SQLITE_ROW)
        {
            if (result.Tasks.size() == query.Limit)
            {
                result.NextAfterId = result.Tasks.back().Id.Value();
                result.NextAfterValue = query.Sort == TaskSummarySort::BoardOrder ? result.Tasks.back().Rank : result.Tasks.back().UpdatedAtUnixMilliseconds;
                break;
            }
            auto row = page.Get();
            TaskSummaryRecord item;
            item.Id = TaskId{ details::ColumnText(row, 0) }; item.Workspace = WorkspaceId{ details::ColumnText(row, 1) };
            item.ProjectName = details::ColumnText(row, 2); item.Title = details::ColumnText(row, 3);
            item.Status = static_cast<TaskStatus>(sqlite3_column_int(row, 4)); item.Priority = static_cast<TaskPriority>(sqlite3_column_int(row, 5));
            item.Version = sqlite3_column_int64(row, 6); item.ManualHold = sqlite3_column_int(row, 7) != 0;
            item.UpdatedAtUnixMilliseconds = sqlite3_column_int64(row, 8); item.Rank = sqlite3_column_int64(row, 9);
            item.ExecutionContextId = details::ColumnText(row, 10); item.AgentId = details::ColumnText(row, 11);
            item.AgentRevision = sqlite3_column_int64(row, 12); item.AgentName = details::ColumnText(row, 13);
            item.Model = details::ColumnText(row, 14); item.Mode = static_cast<ExecutionMode>(sqlite3_column_int(row, 15));
            item.RunId = details::ColumnText(row, 16);
            if (sqlite3_column_type(row, 17) != SQLITE_NULL) item.RunState = static_cast<TaskRunState>(sqlite3_column_int(row, 17));
            item.RunSequence = sqlite3_column_int64(row, 18); item.LastEventSequence = sqlite3_column_int64(row, 19);
            item.HasPendingMessages = sqlite3_column_int(row, 20) != 0; item.HasPendingIntent = sqlite3_column_int(row, 21) != 0;
            item.ArchivedProject = sqlite3_column_type(row, 22) != SQLITE_NULL;
            item.ColumnRevision = sqlite3_column_int64(row, 23);
            result.Tasks.emplace_back(std::move(item));
        }
        if (step != SQLITE_ROW && step != SQLITE_DONE) { result.Error = details::SqliteError(db, step, "read task summary page"); return result; }
        result.Status = TaskStoreCommandStatus::Succeeded;
        return result;
    }
}
