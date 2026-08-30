// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreSchema.h"

#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    namespace
    {
        constexpr const char* SchemaV1 = R"SQL(
CREATE TABLE task (
    id TEXT PRIMARY KEY NOT NULL,
    workspace_id TEXT NOT NULL,
    forked_from_task_id TEXT,
    title TEXT NOT NULL,
    description TEXT NOT NULL,
    status INTEGER NOT NULL CHECK(status BETWEEN 0 AND 6),
    priority INTEGER NOT NULL CHECK(priority BETWEEN 0 AND 2),
    version INTEGER NOT NULL CHECK(version > 0),
    manual_hold INTEGER NOT NULL CHECK(manual_hold IN (0, 1)),
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    updated_at INTEGER NOT NULL CHECK(updated_at >= created_at),
    FOREIGN KEY(forked_from_task_id) REFERENCES task(id) ON DELETE SET NULL
);

CREATE TABLE task_acceptance (
    task_id TEXT NOT NULL,
    position INTEGER NOT NULL CHECK(position >= 0),
    text TEXT NOT NULL,
    satisfied INTEGER NOT NULL CHECK(satisfied IN (0, 1)),
    PRIMARY KEY(task_id, position),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE
);

CREATE TABLE execution_context (
    id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL,
    revision INTEGER NOT NULL CHECK(revision > 0),
    mode INTEGER NOT NULL CHECK(mode BETWEEN 0 AND 1),
    source_root TEXT NOT NULL,
    runtime_root TEXT NOT NULL,
    repository_common_directory TEXT NOT NULL,
    worktree_path TEXT NOT NULL,
    branch TEXT NOT NULL,
    relative_working_directory TEXT NOT NULL,
    provider TEXT NOT NULL,
    model TEXT NOT NULL,
    instructions TEXT NOT NULL,
    identity_frozen INTEGER NOT NULL CHECK(identity_frozen IN (0, 1)),
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    UNIQUE(task_id, revision),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE
);

CREATE TABLE task_terminal (
    id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL UNIQUE,
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 7),
    generation INTEGER NOT NULL CHECK(generation >= 0),
    current_agent_session_id TEXT,
    updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE,
    FOREIGN KEY(current_agent_session_id) REFERENCES agent_session(id) DEFERRABLE INITIALLY DEFERRED
);

CREATE TABLE agent_session (
    id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL,
    provider TEXT NOT NULL,
    provider_session_id TEXT NOT NULL,
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 3),
    is_current INTEGER NOT NULL CHECK(is_current IN (0, 1)),
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    updated_at INTEGER NOT NULL CHECK(updated_at >= created_at),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE
);

CREATE UNIQUE INDEX agent_session_one_current
    ON agent_session(task_id) WHERE is_current = 1;

CREATE TABLE task_run (
    id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL,
    execution_context_id TEXT NOT NULL,
    agent_session_id TEXT,
    retry_of_run_id TEXT,
    trigger INTEGER NOT NULL CHECK(trigger BETWEEN 0 AND 3),
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 8),
    sequence INTEGER NOT NULL CHECK(sequence > 0),
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    started_at INTEGER CHECK(started_at IS NULL OR started_at >= created_at),
    finished_at INTEGER CHECK(finished_at IS NULL OR finished_at >= COALESCE(started_at, created_at)),
    result TEXT NOT NULL,
    reason_code TEXT NOT NULL,
    UNIQUE(task_id, sequence),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE,
    FOREIGN KEY(execution_context_id) REFERENCES execution_context(id),
    FOREIGN KEY(agent_session_id) REFERENCES agent_session(id),
    FOREIGN KEY(retry_of_run_id) REFERENCES task_run(id)
);

CREATE UNIQUE INDEX task_run_one_active
    ON task_run(task_id) WHERE state IN (0, 1, 2, 3, 4);

CREATE TABLE task_message (
    id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL,
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 3),
    revision INTEGER NOT NULL CHECK(revision > 0),
    content TEXT NOT NULL,
    claimed_by_run_id TEXT,
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE,
    FOREIGN KEY(claimed_by_run_id) REFERENCES task_run(id)
);

CREATE TABLE task_run_message (
    run_id TEXT NOT NULL,
    message_id TEXT NOT NULL,
    message_revision INTEGER NOT NULL CHECK(message_revision > 0),
    canonical_content TEXT NOT NULL,
    PRIMARY KEY(run_id, message_id),
    FOREIGN KEY(run_id) REFERENCES task_run(id) ON DELETE CASCADE,
    FOREIGN KEY(message_id) REFERENCES task_message(id)
);

CREATE TABLE task_event (
    id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL,
    run_id TEXT,
    aggregate_sequence INTEGER NOT NULL CHECK(aggregate_sequence > 0),
    run_sequence INTEGER,
    type TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    UNIQUE(task_id, aggregate_sequence),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE,
    FOREIGN KEY(run_id) REFERENCES task_run(id) ON DELETE CASCADE
);

CREATE UNIQUE INDEX task_event_run_sequence
    ON task_event(run_id, run_sequence) WHERE run_id IS NOT NULL;

CREATE TABLE git_checkpoint (
    id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL,
    run_id TEXT NOT NULL,
    state INTEGER NOT NULL,
    commit_id TEXT NOT NULL,
    metadata_json TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE,
    FOREIGN KEY(run_id) REFERENCES task_run(id) ON DELETE CASCADE
);

CREATE TABLE command_receipt (
    id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL,
    command_name TEXT NOT NULL,
    request_digest TEXT NOT NULL,
    expected_version INTEGER NOT NULL CHECK(expected_version >= 0),
    committed_version INTEGER NOT NULL CHECK(committed_version > 0),
    last_event_sequence INTEGER NOT NULL CHECK(last_event_sequence > 0),
    result_json TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE
);

CREATE TABLE delete_tombstone (
    task_id TEXT PRIMARY KEY NOT NULL,
    deleted_at INTEGER NOT NULL CHECK(deleted_at >= 0),
    expires_at INTEGER NOT NULL CHECK(expires_at > deleted_at),
    replay_digest TEXT NOT NULL
);

CREATE INDEX task_workspace_updated ON task(workspace_id, updated_at DESC);
CREATE INDEX task_run_task_order ON task_run(task_id, sequence DESC);
CREATE INDEX task_message_unclaimed ON task_message(task_id, state, created_at);
)SQL";

        constexpr const char* SchemaV2 = R"SQL(
CREATE TABLE task_intent (
    id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL UNIQUE,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 3),
    stage INTEGER NOT NULL CHECK(stage BETWEEN 0 AND 8),
    revision INTEGER NOT NULL CHECK(revision > 0),
    deadline_at INTEGER CHECK(deadline_at IS NULL OR deadline_at >= 0),
    detail_json TEXT NOT NULL,
    reason_code TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    updated_at INTEGER NOT NULL CHECK(updated_at >= created_at),
    FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE
);

CREATE INDEX task_intent_recovery ON task_intent(stage, updated_at, task_id);

ALTER TABLE delete_tombstone ADD COLUMN command_id TEXT;
ALTER TABLE delete_tombstone ADD COLUMN expected_version INTEGER
    CHECK(expected_version IS NULL OR expected_version >= 0);
ALTER TABLE delete_tombstone ADD COLUMN committed_version INTEGER
    CHECK(committed_version IS NULL OR committed_version > 0);
ALTER TABLE delete_tombstone ADD COLUMN last_event_sequence INTEGER
    CHECK(last_event_sequence IS NULL OR last_event_sequence > 0);

CREATE UNIQUE INDEX delete_tombstone_command
    ON delete_tombstone(command_id) WHERE command_id IS NOT NULL;
CREATE INDEX delete_tombstone_expires ON delete_tombstone(expires_at, task_id);
)SQL";

        bool _hasRequiredTables(sqlite3* database, const int32_t version, std::string& error) noexcept
        {
            constexpr auto query = R"SQL(
SELECT count(*)
FROM sqlite_master
WHERE type = 'table'
  AND name IN (
    'task', 'task_acceptance', 'execution_context', 'task_terminal',
    'agent_session', 'task_run', 'task_message', 'task_run_message',
    'task_event', 'git_checkpoint', 'command_receipt', 'delete_tombstone',
    'task_intent'
  );
)SQL";
            const auto count = QueryInt64(database, query, error);
            const auto expectedCount = version >= 2 ? 13 : 12;
            return count && count.value() == expectedCount;
        }

        bool _setSchemaVersion(sqlite3* database, const int32_t version, std::string& error)
        {
            const auto sql = "PRAGMA user_version=" + std::to_string(version) + ";";
            return ExecuteSql(database, sql.c_str(), error);
        }

        bool _migrateVersionOneToTwo(sqlite3* database, std::string& error)
        {
            SqliteTransaction transaction{ database, error };
            return transaction &&
                   ExecuteSql(database, SchemaV2, error) &&
                   _setSchemaVersion(database, 2, error) &&
                   transaction.Commit(error);
        }
    }

    TaskStoreOpenStatus InitializeOrValidateTaskSchema(
        sqlite3* database,
        int32_t& observedVersion,
        std::string& error)
    {
        observedVersion = 0;
        const auto applicationId = QueryInt64(database, "PRAGMA application_id;", error);
        if (!applicationId)
        {
            return TaskStoreOpenStatus::RecoveryRequired;
        }
        auto observedApplicationId = applicationId.value();
        if (observedApplicationId != 0 && observedApplicationId != TaskDatabaseApplicationId)
        {
            error = "Database application_id belongs to another application";
            return TaskStoreOpenStatus::ForeignDatabase;
        }

        const auto version = QueryInt64(database, "PRAGMA user_version;", error);
        if (!version)
        {
            return TaskStoreOpenStatus::RecoveryRequired;
        }
        observedVersion = static_cast<int32_t>(version.value());
        if (observedVersion > CurrentTaskDatabaseSchemaVersion)
        {
            error = "Database schema is newer than this Runtime";
            return TaskStoreOpenStatus::UnsupportedSchema;
        }

        const auto quickCheck = QueryText(database, "PRAGMA quick_check(1);", error);
        if (!quickCheck || quickCheck.value() != "ok")
        {
            if (error.empty())
            {
                error = quickCheck ? quickCheck.value() : "Database quick_check failed";
            }
            return TaskStoreOpenStatus::RecoveryRequired;
        }

        if (observedVersion != 0 && observedApplicationId != TaskDatabaseApplicationId)
        {
            error = "Versioned database is missing the Task database application_id";
            return TaskStoreOpenStatus::ForeignDatabase;
        }

        if (observedVersion == 0)
        {
            const auto existingTables = QueryInt64(
                database,
                "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';",
                error);
            if (!existingTables)
            {
                return TaskStoreOpenStatus::RecoveryRequired;
            }
            if (existingTables.value() != 0)
            {
                error = "Unversioned database already contains tables";
                return TaskStoreOpenStatus::ForeignDatabase;
            }
        }

        if (sqlite3_busy_timeout(database, 5000) != SQLITE_OK ||
            !ExecuteSql(database, "PRAGMA foreign_keys=ON;", error) ||
            !ExecuteSql(database, "PRAGMA journal_mode=WAL;", error) ||
            !ExecuteSql(database, "PRAGMA synchronous=FULL;", error))
        {
            if (error.empty())
            {
                error = "Failed to configure SQLite connection";
            }
            return TaskStoreOpenStatus::IOError;
        }

        if (observedVersion == 0)
        {
            SqliteTransaction transaction{ database, error };
            if (!transaction ||
                !ExecuteSql(database, SchemaV1, error) ||
                !ExecuteSql(database, "PRAGMA application_id=1414744881;", error) ||
                !_setSchemaVersion(database, 1, error) ||
                !transaction.Commit(error))
            {
                return TaskStoreOpenStatus::IOError;
            }
            observedVersion = 1;
            observedApplicationId = TaskDatabaseApplicationId;
        }
        if (!_hasRequiredTables(database, observedVersion, error))
        {
            if (error.empty())
            {
                error = "Task database schema is incomplete";
            }
            return TaskStoreOpenStatus::RecoveryRequired;
        }
        if (observedVersion == 1)
        {
            if (!_migrateVersionOneToTwo(database, error))
            {
                return TaskStoreOpenStatus::IOError;
            }
            observedVersion = 2;
        }
        if (!_hasRequiredTables(database, observedVersion, error))
        {
            if (error.empty())
            {
                error = "Migrated Task database schema is incomplete";
            }
            return TaskStoreOpenStatus::RecoveryRequired;
        }
        return TaskStoreOpenStatus::Ready;
    }
}
