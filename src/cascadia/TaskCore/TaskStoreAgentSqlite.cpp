// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreAgentSqlite.h"
#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    bool MigrateAgentProfileSchema(sqlite3* database, std::string& error)
    {
        constexpr auto sql = R"SQL(
CREATE TABLE agent_profile (
    id TEXT PRIMARY KEY NOT NULL,
    version INTEGER NOT NULL CHECK(version > 0),
    current_revision INTEGER NOT NULL CHECK(current_revision > 0),
    archived_at INTEGER,
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    updated_at INTEGER NOT NULL CHECK(updated_at >= created_at),
    FOREIGN KEY(id, current_revision) REFERENCES agent_profile_revision(agent_id, revision) DEFERRABLE INITIALLY DEFERRED
);
CREATE TABLE agent_profile_revision (
    agent_id TEXT NOT NULL REFERENCES agent_profile(id),
    revision INTEGER NOT NULL CHECK(revision > 0),
    name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 80),
    description TEXT NOT NULL,
    provider TEXT NOT NULL CHECK(provider = 'codex'),
    model TEXT NOT NULL,
    instructions TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    PRIMARY KEY(agent_id, revision)
);
CREATE TABLE workspace_agent_default (
    workspace_id TEXT PRIMARY KEY NOT NULL,
    agent_id TEXT REFERENCES agent_profile(id),
    version INTEGER NOT NULL CHECK(version > 0)
);
CREATE INDEX workspace_agent_default_agent ON workspace_agent_default(agent_id);
CREATE TABLE agent_event (
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    agent_id TEXT REFERENCES agent_profile(id),
    workspace_id TEXT,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 4),
    version INTEGER NOT NULL CHECK(version > 0),
    created_at INTEGER NOT NULL CHECK(created_at >= 0)
);
CREATE TABLE agent_command_receipt (
    command_id TEXT PRIMARY KEY NOT NULL,
    payload_hash TEXT NOT NULL,
    kind INTEGER NOT NULL,
    subject_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    revision INTEGER NOT NULL,
    event_sequence INTEGER NOT NULL REFERENCES agent_event(sequence),
    affected_defaults INTEGER NOT NULL
);
ALTER TABLE execution_context ADD COLUMN source_agent_id TEXT REFERENCES agent_profile(id);
ALTER TABLE execution_context ADD COLUMN source_agent_revision INTEGER NOT NULL DEFAULT 0 CHECK(source_agent_revision >= 0);
ALTER TABLE execution_context ADD COLUMN agent_name_snapshot TEXT NOT NULL DEFAULT '';
CREATE INDEX execution_context_source_agent ON execution_context(source_agent_id);
CREATE TRIGGER execution_context_agent_revision_valid BEFORE INSERT ON execution_context
WHEN (NEW.source_agent_id IS NULL AND (NEW.source_agent_revision != 0 OR NEW.agent_name_snapshot != ''))
 OR (NEW.source_agent_id IS NOT NULL AND NOT EXISTS (
    SELECT 1 FROM agent_profile_revision WHERE agent_id=NEW.source_agent_id AND revision=NEW.source_agent_revision))
BEGIN SELECT RAISE(ABORT, 'Invalid execution Agent revision'); END;
PRAGMA user_version=5;
)SQL";
        SqliteTransaction transaction{ database, error };
        return transaction && ExecuteSql(database, sql, error) && transaction.Commit(error);
    }

    bool ValidateAgentProfileSchema(sqlite3* database, std::string& error)
    {
        const auto count = QueryInt64(database, R"SQL(
SELECT (SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN
 ('agent_profile','agent_profile_revision','workspace_agent_default','agent_event','agent_command_receipt'))
 + (SELECT count(*) FROM pragma_table_info('execution_context') WHERE name IN
 ('source_agent_id','source_agent_revision','agent_name_snapshot'));
)SQL", error);
        if (count && *count == 8) return true;
        if (error.empty()) error = "Task database Agent schema is incomplete";
        return false;
    }

    bool ReadAgentProfile(sqlite3* database, const AgentProfileId& id, std::optional<AgentProfileRecord>& profile, std::string& error, const bool includeDetails)
    {
        SqliteStatement statement{ database, R"SQL(
SELECT p.id,p.version,p.current_revision,p.archived_at,p.created_at,p.updated_at,
 r.name,r.description,r.provider,r.model,CASE WHEN ?2 THEN r.instructions ELSE '' END,
 (SELECT count(*) FROM task_run u JOIN execution_context c ON c.id=u.execution_context_id
  WHERE c.source_agent_id=p.id AND u.state BETWEEN 0 AND 4)
FROM agent_profile p JOIN agent_profile_revision r ON r.agent_id=p.id AND r.revision=p.current_revision
WHERE p.id=?1;
)SQL", error };
        if (!statement || !BindText(database, statement.Get(), 1, id.Value(), error) || !BindInt64(database, statement.Get(), 2, includeDetails, error)) return false;
        const auto stepped = sqlite3_step(statement.Get());
        if (stepped == SQLITE_DONE) { profile.reset(); return true; }
        if (stepped != SQLITE_ROW) { error = SqliteError(database, stepped, "read Agent profile"); return false; }
        AgentProfileRecord row;
        row.Id = AgentProfileId{ ColumnText(statement.Get(), 0) };
        row.Version = sqlite3_column_int64(statement.Get(), 1);
        row.Revision = sqlite3_column_int64(statement.Get(), 2);
        row.Archived = sqlite3_column_type(statement.Get(), 3) != SQLITE_NULL;
        row.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement.Get(), 4);
        row.UpdatedAtUnixMilliseconds = sqlite3_column_int64(statement.Get(), 5);
        row.Content = { ColumnText(statement.Get(), 6), ColumnText(statement.Get(), 7), ColumnText(statement.Get(), 8), ColumnText(statement.Get(), 9), ColumnText(statement.Get(), 10) };
        row.ActiveRuns = static_cast<uint32_t>(sqlite3_column_int64(statement.Get(), 11));
        if (!includeDetails) { profile = std::move(row); return true; }
        SqliteStatement defaults{ database, "SELECT workspace_id FROM workspace_agent_default WHERE agent_id=?1 ORDER BY workspace_id;", error };
        if (!defaults || !BindText(database, defaults.Get(), 1, id.Value(), error)) return false;
        int step;
        while ((step = sqlite3_step(defaults.Get())) == SQLITE_ROW)
            row.DefaultWorkspaces.emplace_back(ColumnText(defaults.Get(), 0));
        if (step != SQLITE_DONE) { error = SqliteError(database, step, "read Agent defaults"); return false; }
        profile = std::move(row);
        return true;
    }

    TaskStoreCommandStatus ResolveExecutionAgent(sqlite3* database, ExecutionContextRecord& context, std::string& error)
    {
        if (!context.SourceAgentId)
        {
            if (context.SourceAgentRevision || !context.AgentNameSnapshot.empty())
            { error = "Custom execution cannot refer to an Agent revision"; return TaskStoreCommandStatus::InvalidRecord; }
            return TaskStoreCommandStatus::Succeeded;
        }
        if (!IsCanonicalUuid(context.SourceAgentId->Value()) || !context.SourceAgentRevision ||
            !context.Model.empty() || !context.Instructions.empty() || !context.AgentNameSnapshot.empty() || context.Provider != "codex")
        { error = "Agent selection cannot include execution overrides"; return TaskStoreCommandStatus::InvalidRecord; }
        std::optional<AgentProfileRecord> profile;
        if (!ReadAgentProfile(database, *context.SourceAgentId, profile, error)) return TaskStoreCommandStatus::StorageError;
        if (!profile) { error = "Agent profile was not found"; return TaskStoreCommandStatus::NotFound; }
        if (profile->Archived || profile->Revision != context.SourceAgentRevision)
        { error = "Agent profile was archived or its configuration changed; reload the selection"; return TaskStoreCommandStatus::VersionConflict; }
        context.Provider = profile->Content.Provider;
        context.Model = profile->Content.Model;
        context.Instructions = profile->Content.Instructions;
        context.AgentNameSnapshot = profile->Content.Name;
        return TaskStoreCommandStatus::Succeeded;
    }

    bool InsertExecutionContext(sqlite3* database, const ExecutionContextRecord& context, std::string& error)
    {
        SqliteStatement statement{ database, R"SQL(
INSERT INTO execution_context(id,task_id,revision,mode,source_root,runtime_root,
 repository_common_directory,worktree_path,branch,relative_working_directory,provider,model,instructions,
 identity_frozen,created_at,source_agent_id,source_agent_revision,agent_name_snapshot)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);
)SQL", error };
        return statement && BindText(database, statement.Get(), 1, context.Id.Value(), error) &&
               BindText(database, statement.Get(), 2, context.Task.Value(), error) &&
               BindInt64(database, statement.Get(), 3, context.Revision, error) &&
               BindInt64(database, statement.Get(), 4, static_cast<int64_t>(context.Mode), error) &&
               BindText(database, statement.Get(), 5, context.SourceRoot, error) &&
               BindText(database, statement.Get(), 6, context.RuntimeRoot, error) &&
               BindText(database, statement.Get(), 7, context.RepositoryCommonDirectory, error) &&
               BindText(database, statement.Get(), 8, context.WorktreePath, error) &&
               BindText(database, statement.Get(), 9, context.Branch, error) &&
               BindText(database, statement.Get(), 10, context.RelativeWorkingDirectory, error) &&
               BindText(database, statement.Get(), 11, context.Provider, error) &&
               BindText(database, statement.Get(), 12, context.Model, error) &&
               BindText(database, statement.Get(), 13, context.Instructions, error) &&
               BindInt64(database, statement.Get(), 14, context.IdentityFrozen, error) &&
               BindInt64(database, statement.Get(), 15, context.CreatedAtUnixMilliseconds, error) &&
               BindOptionalText(database, statement.Get(), 16, context.SourceAgentId ? std::optional{ context.SourceAgentId->Value() } : std::nullopt, error) &&
               BindInt64(database, statement.Get(), 17, context.SourceAgentRevision, error) &&
               BindText(database, statement.Get(), 18, context.AgentNameSnapshot, error) && StepDone(database, statement.Get(), error);
    }
}
