// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskStoreBoardSqlite.h"
#include "TaskStoreSqlite.h"
#include "TaskStoreRunSqlite.h"
#include <limits>

namespace Microsoft::Terminal::TaskCore::details
{
    bool LoadBoardTask(sqlite3* db, const TaskId& id, std::optional<BoardTaskState>& task, std::string& error)
    {
        std::optional<StoredTaskState> state;
        if (!LoadTaskState(db, id, state, error)) return false;
        if (!state) { task.reset(); return true; }
        BoardTaskState value;
        value.Task = *state;
        SqliteStatement row{ db, R"SQL(
SELECT t.workspace_id,p.archived_at,(SELECT id FROM execution_context WHERE task_id=t.id ORDER BY revision DESC LIMIT 1),
 EXISTS(SELECT 1 FROM task_board_operation WHERE task_id=t.id AND stage=0)
FROM task t JOIN project p ON p.workspace_id=t.workspace_id WHERE t.id=?1;
)SQL", error };
        if (!row || !BindText(db, row.Get(), 1, id.Value(), error)) return false;
        const auto step = sqlite3_step(row.Get());
        if (step != SQLITE_ROW) { error = "task_project_missing"; return false; }
        value.Workspace = WorkspaceId{ ColumnText(row.Get(), 0) };
        value.ArchivedProject = sqlite3_column_type(row.Get(), 1) != SQLITE_NULL;
        value.Context = ExecutionContextId{ ColumnText(row.Get(), 2) };
        value.PendingBoardOperation = sqlite3_column_int(row.Get(), 3) != 0;
        if (!LoadActiveTaskRun(db, id, value.ActiveRun, error) || !LoadTaskIntent(db, id, value.Intent, error)) return false;
        task = std::move(value);
        return true;
    }

    bool ReadBoardRevision(sqlite3* db, const WorkspaceId& workspace, const TaskStatus status, uint64_t& revision, std::string& error)
    {
        SqliteStatement row{ db, "SELECT revision FROM project_board_column WHERE workspace_id=?1 AND bucket=?2;", error };
        if (!row || !BindText(db, row.Get(), 1, workspace.Value(), error) || !BindInt64(db, row.Get(), 2, static_cast<int64_t>(TaskBoardBucket(status)), error)) return false;
        if (sqlite3_step(row.Get()) != SQLITE_ROW) { error = "board_column_missing"; return false; }
        revision = sqlite3_column_int64(row.Get(), 0);
        return true;
    }

    bool UpdateBoardTask(sqlite3* db, const TaskId& task, const StoredTaskState& current, TaskStatus status, bool hold, int64_t timestamp, std::string& error)
    {
        if (current.Version >= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) { error = "task_version_exhausted"; return false; }
        SqliteStatement update{ db, "UPDATE task SET status=?1,manual_hold=?2,version=?3,updated_at=?4 WHERE id=?5 AND version=?6;", error };
        return update && BindInt64(db, update.Get(), 1, static_cast<int64_t>(status), error) &&
               BindInt64(db, update.Get(), 2, hold, error) && BindInt64(db, update.Get(), 3, static_cast<int64_t>(current.Version + 1), error) &&
               BindInt64(db, update.Get(), 4, (std::max)(timestamp, current.UpdatedAtUnixMilliseconds), error) &&
               BindText(db, update.Get(), 5, task.Value(), error) && BindInt64(db, update.Get(), 6, static_cast<int64_t>(current.Version), error) &&
               StepDone(db, update.Get(), error) && sqlite3_changes(db) == 1;
    }

    bool PlaceBoardTask(sqlite3* db, const TaskBoardCommand& command, const TaskStatus status, std::string& error)
    {
        std::vector<std::pair<TaskId, int64_t>> rows;
        SqliteStatement list{ db, "SELECT t.id,b.rank FROM task t JOIN task_board_position b ON b.task_id=t.id WHERE t.workspace_id=?1 AND t.status=?2 ORDER BY b.rank,t.id;", error };
        if (!list || !BindText(db, list.Get(), 1, command.Workspace.Value(), error) || !BindInt64(db, list.Get(), 2, static_cast<int64_t>(status), error)) return false;
        int step;
        while ((step = sqlite3_step(list.Get())) == SQLITE_ROW) rows.emplace_back(TaskId{ ColumnText(list.Get(), 0) }, sqlite3_column_int64(list.Get(), 1));
        if (step != SQLITE_DONE) { error = SqliteError(db, step, "read board order"); return false; }
        auto source = std::ranges::find(rows, command.Task, &std::pair<TaskId, int64_t>::first);
        if (source == rows.end()) { error = "board_task_missing"; return false; }
        rows.erase(source);
        auto target = rows.end();
        if (command.Anchor)
        {
            target = std::ranges::find(rows, *command.Anchor, &std::pair<TaskId, int64_t>::first);
            if (target == rows.end()) { error = "board_anchor_changed"; return false; }
            if (command.Placement == BoardPlacement::After) ++target;
        }
        const auto index = static_cast<size_t>(target - rows.begin());
        const auto low = index ? rows[index - 1].second : int64_t{};
        const auto high = index < rows.size() ? rows[index].second : (low < (std::numeric_limits<int64_t>::max)() - 2048 ? low + 2048 : low);
        if (low >= 0 && high > low && high - low > 1)
        {
            SqliteStatement update{ db, "UPDATE task_board_position SET rank=?1 WHERE task_id=?2;", error };
            if (!update || !BindInt64(db, update.Get(), 1, low + (high - low) / 2, error) ||
                !BindText(db, update.Get(), 2, command.Task.Value(), error) || !StepDone(db, update.Get(), error)) return false;
        }
        else
        {
            rows.insert(rows.begin() + static_cast<ptrdiff_t>(index), { command.Task, 0 });
            SqliteStatement update{ db, "UPDATE task_board_position SET rank=?1 WHERE task_id=?2;", error };
            if (!update) return false;
            int64_t rank{};
            for (const auto& row : rows)
            {
                sqlite3_reset(update.Get()); sqlite3_clear_bindings(update.Get());
                rank += 1024;
                if (!BindInt64(db, update.Get(), 1, rank, error) || !BindText(db, update.Get(), 2, row.first.Value(), error) || !StepDone(db, update.Get(), error)) return false;
            }
        }
        SqliteStatement revision{ db, "UPDATE project_board_column SET revision=revision+1 WHERE workspace_id=?1 AND bucket=?2;", error };
        return revision && BindText(db, revision.Get(), 1, command.Workspace.Value(), error) &&
               BindInt64(db, revision.Get(), 2, static_cast<int64_t>(TaskBoardBucket(status)), error) && StepDone(db, revision.Get(), error);
    }

    bool StartBoardRun(sqlite3* db, const TaskBoardCommand& command, const BoardTaskState& task, std::string& error)
    {
        TaskRunRecord run;
        run.Id = command.NewRun; run.Task = command.Task; run.ExecutionContext = task.Context;
        run.State = TaskRunState::Queued; run.Trigger = command.Effect == TaskBoardEffect::Continue ? TaskRunTrigger::FollowUp : TaskRunTrigger::Initial;
        run.CreatedAtUnixMilliseconds = command.Metadata.TimestampUnixMilliseconds;
        if (ValidateTaskRunReferences(db, run, error) != TaskRunReferenceValidation::Valid) return false;
        if (!command.Text.empty())
        {
            TaskMessageRecord message;
            message.Id = command.NewMessage; message.Task = command.Task; message.Content = command.Text;
            message.Author = TaskMessageAuthor::User; message.Revision = 1;
            message.CreatedAtUnixMilliseconds = message.UpdatedAtUnixMilliseconds = command.Metadata.TimestampUnixMilliseconds;
            if (!InsertTaskMessage(db, message, TaskMessageState::Pending, std::nullopt, error)) return false;
        }
        std::vector<TaskMessageRecord> messages;
        bool exceeds{};
        if (!LoadClaimableTaskMessages(db, command.Task, MaximumTaskMessagesPerRun, messages, exceeds, error)) return false;
        if (exceeds) { error = "task_message_capacity"; return false; }
        uint64_t sequence{};
        if (!NextRunSequence(db, command.Task, sequence, error) || !InsertTaskRun(db, run, sequence, error)) return false;
        uint64_t position{};
        for (const auto& message : messages)
        {
            if (!ClaimTaskMessage(db, message, run.Id, command.Metadata.TimestampUnixMilliseconds, error) ||
                !InsertTaskRunMessage(db, run.Id, message, position++, error)) return false;
        }
        return true;
    }

    bool BeginBoardIntent(sqlite3* db, const TaskBoardCommand& command, TaskIntentKind kind, bool done, std::string& error)
    {
        SqliteStatement insert{ db, R"SQL(
INSERT INTO task_intent(id,task_id,kind,stage,revision,deadline_at,detail_json,reason_code,created_at,updated_at)
VALUES(?1,?2,?3,0,1,NULL,?4,'',?5,?5)
ON CONFLICT(task_id) DO UPDATE SET id=excluded.id,kind=excluded.kind,stage=0,revision=1,deadline_at=NULL,
 detail_json=excluded.detail_json,reason_code='',created_at=excluded.created_at,updated_at=excluded.updated_at WHERE task_intent.stage=8;
)SQL", error };
        return insert && BindText(db, insert.Get(), 1, command.Intent.Value(), error) && BindText(db, insert.Get(), 2, command.Task.Value(), error) &&
               BindInt64(db, insert.Get(), 3, static_cast<int64_t>(kind), error) &&
               BindText(db, insert.Get(), 4, done ? R"({"done":true,"source":"task_board"})" : R"({"source":"task_board"})", error) &&
               BindInt64(db, insert.Get(), 5, command.Metadata.TimestampUnixMilliseconds, error) && StepDone(db, insert.Get(), error) && sqlite3_changes(db) == 1;
    }
}
