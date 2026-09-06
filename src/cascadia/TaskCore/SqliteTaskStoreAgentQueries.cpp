// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStoreInternal.h"
#include "TaskStoreAgentSqlite.h"
#include "TaskStoreSqlite.h"
#include <wil/resource.h>

namespace Microsoft::Terminal::TaskCore
{
    AgentProfileQueryResult SqliteTaskStore::QueryAgentProfiles(const AgentProfileQuery& query) const
    {
        AgentProfileQueryResult result;
        if (!IsReady()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = LastError(); return result; }
        if (!query.Limit || query.Limit > MaximumAgentProfilePageSize ||
            (query.Agent && !IsCanonicalUuid(query.Agent->Value())) ||
            (query.Workspace && !IsCanonicalUuid(query.Workspace->Value())) ||
            (!query.AfterId.empty() && !IsCanonicalUuid(query.AfterId)))
        { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = "Agent query is invalid"; return result; }
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        auto db = _impl->Database;
        // One read transaction keeps the page, default binding and cursor consistent.
        if (!details::ExecuteSql(db, "BEGIN;", result.Error)) return result;
        const auto rollback = wil::scope_exit([&] { std::string ignored; static_cast<void>(details::ExecuteSql(db, "ROLLBACK;", ignored)); });
        const auto generation = details::QueryInt64(db, "SELECT coalesce(max(sequence),0) FROM agent_event;", result.Error);
        if (!generation) return result;
        result.Generation = *generation;
        if (query.Workspace)
        {
            result.Default = WorkspaceAgentDefault{ *query.Workspace };
            details::SqliteStatement statement{ db, "SELECT agent_id,version FROM workspace_agent_default WHERE workspace_id=?1;", result.Error };
            if (!statement || !details::BindText(db, statement.Get(), 1, query.Workspace->Value(), result.Error)) return result;
            const auto step = sqlite3_step(statement.Get());
            if (step == SQLITE_ROW)
            {
                const auto agent = details::ColumnOptionalText(statement.Get(), 0);
                if (agent) result.Default->Agent = AgentProfileId{ *agent };
                result.Default->Version = sqlite3_column_int64(statement.Get(), 1);
            }
            else if (step != SQLITE_DONE) { result.Error = details::SqliteError(db, step, "query workspace Agent"); return result; }
        }
        if (query.Agent)
        {
            std::optional<AgentProfileRecord> profile;
            if (!details::ReadAgentProfile(db, *query.Agent, profile, result.Error)) return result;
            if (!profile) { result.Status = TaskStoreCommandStatus::NotFound; result.Error = "Agent was not found"; return result; }
            result.Profiles.emplace_back(std::move(*profile));
        }
        else
        {
            details::SqliteStatement statement{ db, "SELECT id FROM agent_profile WHERE id>?1 AND (?2 OR archived_at IS NULL) ORDER BY id LIMIT ?3;", result.Error };
            if (!statement || !details::BindText(db, statement.Get(), 1, query.AfterId, result.Error) ||
                !details::BindInt64(db, statement.Get(), 2, query.IncludeArchived, result.Error) ||
                !details::BindInt64(db, statement.Get(), 3, query.Limit + 1, result.Error)) return result;
            int step;
            while ((step = sqlite3_step(statement.Get())) == SQLITE_ROW)
            {
                if (result.Profiles.size() == query.Limit)
                { result.NextAfterId = result.Profiles.back().Id.Value(); break; }
                std::optional<AgentProfileRecord> profile;
                if (!details::ReadAgentProfile(db, AgentProfileId{ details::ColumnText(statement.Get(), 0) }, profile, result.Error, false) || !profile) return result;
                // Lists do not expose potentially large editor fields.
                profile->Content.Instructions.clear();
                profile->DefaultWorkspaces.clear();
                result.Profiles.emplace_back(std::move(*profile));
            }
            if (step != SQLITE_ROW && step != SQLITE_DONE) { result.Error = details::SqliteError(db, step, "list Agents"); return result; }
        }
        result.Status = TaskStoreCommandStatus::Succeeded;
        return result;
    }
}
