// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskStoreProjectSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    bool MigrateProjectSchema(sqlite3* database, std::string& error)
    {
        constexpr auto sql = R"SQL(
CREATE TABLE project (
 workspace_id TEXT PRIMARY KEY NOT NULL, version INTEGER NOT NULL DEFAULT 1 CHECK(typeof(version)='integer' AND version>0),
 name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', kind INTEGER NOT NULL DEFAULT 0 CHECK(kind IN(0,1)),
 binding_state INTEGER NOT NULL DEFAULT 0 CHECK(binding_state IN(0,1)), root_path TEXT NOT NULL DEFAULT '',
 root_identity TEXT NOT NULL DEFAULT '', archived_at INTEGER, legacy INTEGER NOT NULL DEFAULT 1 CHECK(legacy IN(0,1)),
 created_at INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL DEFAULT 0,
 CHECK(updated_at>=created_at), CHECK(binding_state=0 OR (length(root_path)>0 AND length(root_identity)=64))
);
CREATE INDEX project_directory ON project(root_identity) WHERE binding_state=1;
CREATE TABLE project_board_column (
 workspace_id TEXT NOT NULL REFERENCES project(workspace_id), bucket INTEGER NOT NULL CHECK(bucket IN(0,1,2,3,5,6)),
 revision INTEGER NOT NULL DEFAULT 1 CHECK(typeof(revision)='integer' AND revision>0), PRIMARY KEY(workspace_id,bucket)
);
CREATE TABLE task_board_position (task_id TEXT PRIMARY KEY NOT NULL REFERENCES task(id) ON DELETE CASCADE, rank INTEGER NOT NULL CHECK(typeof(rank)='integer'));
CREATE TABLE project_event (
 sequence INTEGER PRIMARY KEY AUTOINCREMENT, workspace_id TEXT NOT NULL, task_id TEXT,
 kind TEXT NOT NULL, version INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE project_command_receipt (
 command_id TEXT PRIMARY KEY NOT NULL, payload_hash TEXT NOT NULL, requested_id TEXT NOT NULL,
 subject_id TEXT NOT NULL, kind INTEGER NOT NULL, version INTEGER NOT NULL, event_sequence INTEGER NOT NULL,
 existing INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE board_command_receipt (
 command_id TEXT PRIMARY KEY NOT NULL, payload_hash TEXT NOT NULL, task_id TEXT NOT NULL,
 task_version INTEGER NOT NULL, column_revision INTEGER NOT NULL, event_sequence INTEGER NOT NULL,
 run_id TEXT, operation_id TEXT, pending INTEGER NOT NULL
);
CREATE TABLE task_board_note (task_id TEXT PRIMARY KEY NOT NULL REFERENCES task(id) ON DELETE CASCADE, text TEXT NOT NULL, kind INTEGER NOT NULL CHECK(kind IN(0,1)), created_at INTEGER NOT NULL);
CREATE TABLE task_owned_session (sequence INTEGER PRIMARY KEY AUTOINCREMENT, provider_session_id TEXT NOT NULL UNIQUE);
INSERT OR IGNORE INTO task_owned_session(provider_session_id)
 SELECT provider_session_id FROM agent_session WHERE provider='codex' AND provider_session_id<>'';
CREATE TRIGGER task_owned_session_created AFTER INSERT ON agent_session WHEN NEW.provider='codex' AND NEW.provider_session_id<>'' BEGIN
 INSERT OR IGNORE INTO task_owned_session(provider_session_id) VALUES(NEW.provider_session_id);
END;
CREATE TRIGGER task_owned_session_changed AFTER UPDATE OF provider_session_id ON agent_session WHEN NEW.provider='codex' AND NEW.provider_session_id<>'' BEGIN
 INSERT OR IGNORE INTO task_owned_session(provider_session_id) VALUES(NEW.provider_session_id);
END;
CREATE TABLE task_board_undo (
 command_id TEXT PRIMARY KEY NOT NULL, task_id TEXT NOT NULL REFERENCES task(id) ON DELETE CASCADE,
 effect INTEGER NOT NULL CHECK(effect IN(0,1)), source_status INTEGER NOT NULL, target_status INTEGER NOT NULL,
 original_hold INTEGER NOT NULL CHECK(original_hold IN(0,1)), next_task_id TEXT, task_version INTEGER NOT NULL,
 source_revision INTEGER NOT NULL, target_revision INTEGER NOT NULL,
 note TEXT, note_kind INTEGER NOT NULL, note_created_at INTEGER NOT NULL, undone_by TEXT
);
CREATE TABLE task_board_operation (
 operation_id TEXT PRIMARY KEY NOT NULL, command_id TEXT NOT NULL UNIQUE, task_id TEXT NOT NULL REFERENCES task(id) ON DELETE CASCADE,
 related_run_id TEXT, intent_id TEXT, source_status INTEGER NOT NULL, target_status INTEGER NOT NULL,
 stage INTEGER NOT NULL DEFAULT 0 CHECK(stage BETWEEN 0 AND 2), reason TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL
);
ALTER TABLE execution_context ADD COLUMN project_version INTEGER NOT NULL DEFAULT 0;
ALTER TABLE execution_context ADD COLUMN project_name_snapshot TEXT NOT NULL DEFAULT '';
ALTER TABLE execution_context ADD COLUMN project_description_snapshot TEXT NOT NULL DEFAULT '';
CREATE INDEX task_workspace_status ON task(workspace_id,status);
CREATE INDEX project_event_scope ON project_event(workspace_id,sequence);
CREATE INDEX board_operation_pending ON task_board_operation(stage,task_id);

CREATE TRIGGER project_inserted AFTER INSERT ON project BEGIN
 INSERT INTO project_board_column(workspace_id,bucket) VALUES(NEW.workspace_id,0),(NEW.workspace_id,1),(NEW.workspace_id,2),(NEW.workspace_id,3),(NEW.workspace_id,5),(NEW.workspace_id,6);
 INSERT INTO project_event(workspace_id,kind,version,created_at) VALUES(NEW.workspace_id,'project_created',NEW.version,NEW.updated_at);
END;
CREATE TRIGGER project_changed AFTER UPDATE ON project BEGIN
 INSERT INTO project_event(workspace_id,kind,version,created_at) VALUES(NEW.workspace_id,'project_changed',NEW.version,NEW.updated_at);
END;
INSERT INTO project(workspace_id,name,created_at,updated_at)
 SELECT workspace_id,'Workspace '||substr(workspace_id,1,8),min(created_at),max(updated_at) FROM task GROUP BY workspace_id;
INSERT OR IGNORE INTO project(workspace_id,name)
 SELECT workspace_id,'Workspace '||substr(workspace_id,1,8) FROM workspace_agent_default;
INSERT INTO task_board_position(task_id,rank)
 SELECT id,1024*row_number() OVER(PARTITION BY workspace_id,CASE status WHEN 4 THEN 2 ELSE status END ORDER BY updated_at DESC,id) FROM task;

CREATE TRIGGER task_project_not_archived BEFORE INSERT ON task
WHEN EXISTS(SELECT 1 FROM project WHERE workspace_id=NEW.workspace_id AND archived_at IS NOT NULL)
BEGIN SELECT RAISE(ABORT,'project_archived'); END;
CREATE TRIGGER task_project_created AFTER INSERT ON task BEGIN
 INSERT OR IGNORE INTO project(workspace_id,name,created_at,updated_at) VALUES(NEW.workspace_id,'Workspace '||substr(NEW.workspace_id,1,8),NEW.created_at,NEW.updated_at);
 INSERT INTO task_board_position(task_id,rank)
 SELECT NEW.id,coalesce(max(b.rank),0)+1024 FROM task_board_position b JOIN task t ON t.id=b.task_id
 WHERE t.workspace_id=NEW.workspace_id AND (CASE t.status WHEN 4 THEN 2 ELSE t.status END)=(CASE NEW.status WHEN 4 THEN 2 ELSE NEW.status END);
 UPDATE project_board_column SET revision=revision+1 WHERE workspace_id=NEW.workspace_id AND bucket=(CASE NEW.status WHEN 4 THEN 2 ELSE NEW.status END);
 INSERT INTO project_event(workspace_id,task_id,kind,version,created_at) VALUES(NEW.workspace_id,NEW.id,'task_created',NEW.version,NEW.updated_at);
END;
CREATE TRIGGER task_board_status_changed AFTER UPDATE OF status ON task WHEN OLD.status!=NEW.status BEGIN
 UPDATE task_board_position SET rank=(SELECT coalesce(max(b.rank),0)+1024 FROM task_board_position b JOIN task t ON t.id=b.task_id
 WHERE t.workspace_id=NEW.workspace_id AND t.id!=NEW.id AND (CASE t.status WHEN 4 THEN 2 ELSE t.status END)=(CASE NEW.status WHEN 4 THEN 2 ELSE NEW.status END)) WHERE task_id=NEW.id;
 UPDATE project_board_column SET revision=revision+1 WHERE workspace_id=NEW.workspace_id AND bucket IN(CASE OLD.status WHEN 4 THEN 2 ELSE OLD.status END,CASE NEW.status WHEN 4 THEN 2 ELSE NEW.status END);
 INSERT INTO project_event(workspace_id,task_id,kind,version,created_at) VALUES(NEW.workspace_id,NEW.id,'task_status',NEW.version,NEW.updated_at);
END;
CREATE TRIGGER task_board_deleted AFTER DELETE ON task BEGIN
 UPDATE project_board_column SET revision=revision+1 WHERE workspace_id=OLD.workspace_id AND bucket=(CASE OLD.status WHEN 4 THEN 2 ELSE OLD.status END);
 INSERT INTO project_event(workspace_id,task_id,kind,version,created_at) VALUES(OLD.workspace_id,OLD.id,'task_deleted',OLD.version,OLD.updated_at);
END;
CREATE TRIGGER agent_default_project AFTER INSERT ON workspace_agent_default BEGIN
 INSERT OR IGNORE INTO project(workspace_id,name) VALUES(NEW.workspace_id,'Workspace '||substr(NEW.workspace_id,1,8));
 INSERT INTO project_event(workspace_id,kind,version) VALUES(NEW.workspace_id,'agent_default',NEW.version);
END;
CREATE TRIGGER agent_default_project_changed AFTER UPDATE ON workspace_agent_default BEGIN
 INSERT INTO project_event(workspace_id,kind,version) VALUES(NEW.workspace_id,'agent_default',NEW.version);
END;
CREATE TRIGGER project_run_guard BEFORE INSERT ON task_run
WHEN EXISTS(SELECT 1 FROM task t JOIN project p ON p.workspace_id=t.workspace_id WHERE t.id=NEW.task_id AND p.archived_at IS NOT NULL)
 OR EXISTS(SELECT 1 FROM task_board_operation WHERE task_id=NEW.task_id AND stage=0)
BEGIN SELECT RAISE(ABORT,'project_archived_or_board_operation_pending'); END;
CREATE TRIGGER project_message_guard BEFORE INSERT ON task_message
WHEN NEW.author=0 AND EXISTS(SELECT 1 FROM task t JOIN project p ON p.workspace_id=t.workspace_id WHERE t.id=NEW.task_id AND p.archived_at IS NOT NULL)
BEGIN SELECT RAISE(ABORT,'project_archived'); END;
CREATE TRIGGER project_run_created AFTER INSERT ON task_run BEGIN
 DELETE FROM task_board_note WHERE task_id=NEW.task_id;
 INSERT INTO project_event(workspace_id,task_id,kind,version,created_at)
 SELECT workspace_id,id,'run_created',version,NEW.created_at FROM task WHERE id=NEW.task_id;
END;
CREATE TRIGGER project_run_changed AFTER UPDATE OF state ON task_run WHEN OLD.state!=NEW.state BEGIN
 INSERT INTO project_event(workspace_id,task_id,kind,version,created_at)
 SELECT workspace_id,id,'run_state',version,updated_at FROM task WHERE id=NEW.task_id;
END;
PRAGMA user_version=6;
)SQL";
        SqliteTransaction transaction{ database, error };
        return transaction && ExecuteSql(database, sql, error) && transaction.Commit(error);
    }

    bool ValidateProjectSchema(sqlite3* database, std::string& error)
    {
        const auto count = QueryInt64(database, R"SQL(
SELECT (SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN
 ('project','project_board_column','task_board_position','project_event','project_command_receipt','board_command_receipt','task_board_operation','task_board_note','task_board_undo','task_owned_session'))
 +(SELECT count(*) FROM pragma_table_info('execution_context') WHERE name IN('project_version','project_name_snapshot','project_description_snapshot'))
 +(SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name IN('project_inserted','project_changed','task_project_not_archived','task_project_created','task_board_status_changed','task_board_deleted','agent_default_project','agent_default_project_changed','project_run_guard','project_message_guard','project_run_created','project_run_changed','task_owned_session_created','task_owned_session_changed'));
)SQL", error);
        if (count && *count == 27) return true;
        if (error.empty()) error = "Project database schema is incomplete";
        return false;
    }
}
