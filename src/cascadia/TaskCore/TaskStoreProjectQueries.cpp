// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "SqliteTaskStoreInternal.h"
#include "TaskStoreProjectSqlite.h"
#include <wil/resource.h>

namespace Microsoft::Terminal::TaskCore::details
{
    bool ReadProject(sqlite3* db, const WorkspaceId& id, std::optional<ProjectRecord>& result, std::string& error)
    {
        SqliteStatement row{ db, R"SQL(
SELECT p.workspace_id,p.version,p.name,p.description,p.kind,p.binding_state,p.root_path,p.root_identity,
 p.archived_at,p.legacy,p.created_at,p.updated_at,d.agent_id,
 (SELECT count(*) FROM task_run r JOIN task t ON t.id=r.task_id WHERE t.workspace_id=p.workspace_id AND r.state BETWEEN 0 AND 4)
FROM project p LEFT JOIN workspace_agent_default d ON d.workspace_id=p.workspace_id WHERE p.workspace_id=?1;
)SQL", error };
        if (!row || !BindText(db, row.Get(), 1, id.Value(), error)) return false;
        const auto step = sqlite3_step(row.Get());
        if (step == SQLITE_DONE) { result.reset(); return true; }
        if (step != SQLITE_ROW) { error = SqliteError(db, step, "read project"); return false; }
        ProjectRecord project;
        project.Id = WorkspaceId{ ColumnText(row.Get(), 0) };
        project.Version = sqlite3_column_int64(row.Get(), 1);
        project.Name = ColumnText(row.Get(), 2);
        project.Description = ColumnText(row.Get(), 3);
        project.Kind = static_cast<ProjectKind>(sqlite3_column_int(row.Get(), 4));
        project.Binding = static_cast<ProjectBindingState>(sqlite3_column_int(row.Get(), 5));
        project.RootPath = ColumnText(row.Get(), 6);
        project.RootIdentity = ColumnText(row.Get(), 7);
        project.Archived = sqlite3_column_type(row.Get(), 8) != SQLITE_NULL;
        project.Legacy = sqlite3_column_int(row.Get(), 9) != 0;
        project.CreatedAtUnixMilliseconds = sqlite3_column_int64(row.Get(), 10);
        project.UpdatedAtUnixMilliseconds = sqlite3_column_int64(row.Get(), 11);
        if (const auto agent = ColumnOptionalText(row.Get(), 12)) project.DefaultAgent = AgentProfileId{ *agent };
        project.ActiveRuns = sqlite3_column_int64(row.Get(), 13);
        SqliteStatement counts{ db, "SELECT status,count(*) FROM task WHERE workspace_id=?1 GROUP BY status;", error };
        if (!counts || !BindText(db, counts.Get(), 1, id.Value(), error)) return false;
        int counted;
        while ((counted = sqlite3_step(counts.Get())) == SQLITE_ROW)
        {
            const auto status = sqlite3_column_int(counts.Get(), 0);
            if (status < 0 || status >= 7) { error = "Invalid stored Task status"; return false; }
            project.StatusCounts[static_cast<size_t>(status)] = sqlite3_column_int64(counts.Get(), 1);
        }
        if (counted != SQLITE_DONE) { error = SqliteError(db, counted, "count project tasks"); return false; }
        SqliteStatement columns{ db, "SELECT bucket,revision FROM project_board_column WHERE workspace_id=?1;", error };
        if (!columns || !BindText(db, columns.Get(), 1, id.Value(), error)) return false;
        while ((counted = sqlite3_step(columns.Get())) == SQLITE_ROW)
        {
            const auto bucket = sqlite3_column_int(columns.Get(), 0);
            if (bucket < 0 || bucket > 6) { error = "Invalid board bucket"; return false; }
            project.ColumnRevisions[static_cast<size_t>(bucket)] = sqlite3_column_int64(columns.Get(), 1);
        }
        if (counted != SQLITE_DONE) { error = SqliteError(db, counted, "read project columns"); return false; }
        project.ColumnRevisions[4] = project.ColumnRevisions[2];
        result = std::move(project);
        return true;
    }

    bool ProjectHasWork(sqlite3* db, const WorkspaceId& id, bool& result, std::string& error)
    {
        SqliteStatement statement{ db, R"SQL(
SELECT EXISTS(SELECT 1 FROM task t WHERE t.workspace_id=?1 AND (
 EXISTS(SELECT 1 FROM task_run r WHERE r.task_id=t.id AND r.state BETWEEN 0 AND 4)
 OR EXISTS(SELECT 1 FROM task_intent i WHERE i.task_id=t.id AND i.stage!=8)
 OR EXISTS(SELECT 1 FROM task_board_operation o WHERE o.task_id=t.id AND o.stage=0)
 OR (t.manual_hold=0 AND EXISTS(SELECT 1 FROM task_message m WHERE m.task_id=t.id AND m.state IN(0,1)))
));
)SQL", error };
        if (!statement || !BindText(db, statement.Get(), 1, id.Value(), error)) return false;
        const auto step = sqlite3_step(statement.Get());
        if (step != SQLITE_ROW) { error = SqliteError(db, step, "check project activity"); return false; }
        result = sqlite3_column_int(statement.Get(), 0) != 0;
        return true;
    }
}

namespace Microsoft::Terminal::TaskCore
{
    ProjectQueryResult SqliteTaskStore::QueryProjects(const ProjectQuery& query) const
    {
        ProjectQueryResult result;
        if (!IsReady()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = LastError(); return result; }
        if (!query.Limit || query.Limit > MaximumProjectPageSize || (query.Id && !IsCanonicalUuid(query.Id->Value())) ||
            (!query.AfterId.empty() && !IsCanonicalUuid(query.AfterId)))
        { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = "project_invalid_query"; return result; }
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        auto db = _impl->Database;
        if (!details::ExecuteSql(db, "BEGIN;", result.Error)) return result;
        const auto rollback = wil::scope_exit([&] { std::string ignored; static_cast<void>(details::ExecuteSql(db, "ROLLBACK;", ignored)); });
        const auto generation = details::QueryInt64(db, "SELECT coalesce(max(sequence),0) FROM project_event;", result.Error);
        if (!generation) return result;
        result.Generation = *generation;
        if (query.Id)
        {
            std::optional<ProjectRecord> project;
            if (!details::ReadProject(db, *query.Id, project, result.Error)) return result;
            if (!project) { result.Status = TaskStoreCommandStatus::NotFound; result.Error = "project_not_found"; return result; }
            result.Projects.emplace_back(std::move(*project));
        }
        else
        {
            details::SqliteStatement page{ db, "SELECT workspace_id FROM project WHERE workspace_id>?1 AND (?2 OR archived_at IS NULL) ORDER BY workspace_id LIMIT ?3;", result.Error };
            if (!page || !details::BindText(db, page.Get(), 1, query.AfterId, result.Error) ||
                !details::BindInt64(db, page.Get(), 2, query.IncludeArchived, result.Error) ||
                !details::BindInt64(db, page.Get(), 3, static_cast<int64_t>(query.Limit + 1), result.Error)) return result;
            int step;
            size_t bytes{};
            while ((step = sqlite3_step(page.Get())) == SQLITE_ROW)
            {
                if (result.Projects.size() == query.Limit || bytes > 384 * 1024)
                { result.NextAfterId = result.Projects.back().Id.Value(); break; }
                std::optional<ProjectRecord> project;
                if (!details::ReadProject(db, WorkspaceId{ details::ColumnText(page.Get(), 0) }, project, result.Error) || !project) return result;
                project->Description.clear();
                bytes += project->RootPath.size() + project->Name.size() + 1024;
                result.Projects.emplace_back(std::move(*project));
            }
            if (step != SQLITE_ROW && step != SQLITE_DONE) { result.Error = details::SqliteError(db, step, "list projects"); return result; }
        }
        result.Status = TaskStoreCommandStatus::Succeeded;
        return result;
    }
}
