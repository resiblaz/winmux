// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskStoreBoardSqlite.h"
#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    bool CaptureBoardUndo(sqlite3* db, const TaskId& task, BoardUndoSnapshot& snapshot, std::string& error)
    {
        SqliteStatement row{ db, R"SQL(
SELECT (SELECT t.id FROM task t JOIN task_board_position p ON p.task_id=t.id
        WHERE t.workspace_id=source.workspace_id AND t.status=source.status
          AND (p.rank>position.rank OR (p.rank=position.rank AND t.id>source.id))
        ORDER BY p.rank,t.id LIMIT 1), note.text,coalesce(note.kind,0),coalesce(note.created_at,0)
FROM task source JOIN task_board_position position ON position.task_id=source.id
LEFT JOIN task_board_note note ON note.task_id=source.id WHERE source.id=?1;
)SQL", error };
        if (!row || !BindText(db, row.Get(), 1, task.Value(), error)) return false;
        const auto step = sqlite3_step(row.Get());
        if (step != SQLITE_ROW) { error = SqliteError(db, step, "capture board undo"); return false; }
        snapshot.NextTask = ColumnOptionalText(row.Get(), 0); snapshot.Note = ColumnOptionalText(row.Get(), 1);
        snapshot.NoteKind = sqlite3_column_int64(row.Get(), 2); snapshot.NoteCreatedAt = sqlite3_column_int64(row.Get(), 3);
        return true;
    }
    bool SaveBoardUndo(sqlite3* db, const TaskBoardCommand& command, const BoardTaskState& before,
                       const BoardUndoSnapshot& snapshot, const TaskBoardCommandResult& after, std::string& error)
    {
        uint64_t sourceRevision{};
        if (!ReadBoardRevision(db, command.Workspace, before.Task.Status, sourceRevision, error)) return false;
        SqliteStatement row{ db, R"SQL(
INSERT INTO task_board_undo(command_id,task_id,effect,source_status,target_status,original_hold,next_task_id,task_version,
                           source_revision,target_revision,note,note_kind,note_created_at)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);
)SQL", error };
        return row && BindText(db, row.Get(), 1, command.Metadata.Command.Value(), error) && BindText(db, row.Get(), 2, command.Task.Value(), error) &&
               BindInt64(db, row.Get(), 3, static_cast<int64_t>(command.Effect), error) && BindInt64(db, row.Get(), 4, static_cast<int64_t>(before.Task.Status), error) &&
               BindInt64(db, row.Get(), 5, static_cast<int64_t>(command.Target), error) && BindInt64(db, row.Get(), 6, before.Task.ManualHold, error) &&
               BindOptionalText(db, row.Get(), 7, snapshot.NextTask, error) && BindInt64(db, row.Get(), 8, static_cast<int64_t>(after.TaskVersion), error) &&
               BindInt64(db, row.Get(), 9, static_cast<int64_t>(sourceRevision), error) && BindInt64(db, row.Get(), 10, static_cast<int64_t>(after.ColumnRevision), error) &&
               BindOptionalText(db, row.Get(), 11, snapshot.Note, error) && BindInt64(db, row.Get(), 12, snapshot.NoteKind, error) &&
               BindInt64(db, row.Get(), 13, snapshot.NoteCreatedAt, error) && StepDone(db, row.Get(), error);
    }
    TaskBoardCommandResult ApplyBoardUndo(sqlite3* db, const TaskBoardCommand& command, const BoardTaskState& current)
    {
        TaskBoardCommandResult result; result.Task = command.Task;
        auto& error = result.Error;
        const auto fail = [&](TaskStoreCommandStatus status, std::string message) { result.Status = status; result.Error = std::move(message); return result; };
        SqliteStatement row{ db, R"SQL(
SELECT effect,source_status,target_status,original_hold,next_task_id,task_version,source_revision,target_revision,note,note_kind,note_created_at,undone_by
FROM task_board_undo WHERE command_id=?1 AND task_id=?2;
)SQL", error };
        if (!row || !BindText(db, row.Get(), 1, command.UndoOf->Value(), error) || !BindText(db, row.Get(), 2, command.Task.Value(), error)) return result;
        const auto step = sqlite3_step(row.Get());
        if (step == SQLITE_DONE) return fail(TaskStoreCommandStatus::InvalidTransition, "board_action_cannot_be_undone");
        if (step != SQLITE_ROW) { error = SqliteError(db, step, "read board undo"); return result; }
        const auto reorder = sqlite3_column_int(row.Get(), 0) == static_cast<int>(TaskBoardEffect::Reorder);
        const auto source = static_cast<TaskStatus>(sqlite3_column_int(row.Get(), 1));
        const auto target = static_cast<TaskStatus>(sqlite3_column_int(row.Get(), 2));
        const auto hold = sqlite3_column_int(row.Get(), 3) != 0;
        const auto next = ColumnOptionalText(row.Get(), 4);
        const auto note = ColumnOptionalText(row.Get(), 8);
        const auto noteKind = sqlite3_column_int64(row.Get(), 9), noteTime = sqlite3_column_int64(row.Get(), 10);
        if (sqlite3_column_type(row.Get(), 11) != SQLITE_NULL || current.Task.Status != target ||
            current.Task.Version != static_cast<uint64_t>(sqlite3_column_int64(row.Get(), 5)) || current.Task.Version != command.Metadata.ExpectedTaskVersion)
            return fail(TaskStoreCommandStatus::VersionConflict, "board_undo_task_changed");
        if (current.PendingBoardOperation || (!reorder && (current.ActiveRun || (current.Intent && current.Intent->Stage != TaskIntentStage::Completed))))
            return fail(TaskStoreCommandStatus::ActiveIntentExists, "board_undo_work_in_progress");
        uint64_t sourceRevision{}, targetRevision{};
        if (!ReadBoardRevision(db, command.Workspace, source, sourceRevision, error) || !ReadBoardRevision(db, command.Workspace, target, targetRevision, error)) return result;
        if (sourceRevision != static_cast<uint64_t>(sqlite3_column_int64(row.Get(), 6)) || targetRevision != static_cast<uint64_t>(sqlite3_column_int64(row.Get(), 7)))
            return fail(TaskStoreCommandStatus::VersionConflict, "board_undo_column_changed");
        result.TaskVersion = current.Task.Version;
        if (!reorder)
        {
            if (!UpdateBoardTask(db, command.Task, current.Task, source, hold, command.Metadata.TimestampUnixMilliseconds, error)) return result;
            ++result.TaskVersion;
            SqliteStatement clear{ db, "DELETE FROM task_board_note WHERE task_id=?1;", error };
            if (!clear || !BindText(db, clear.Get(), 1, command.Task.Value(), error) || !StepDone(db, clear.Get(), error)) return result;
            if (note)
            {
                SqliteStatement restore{ db, "INSERT INTO task_board_note(task_id,text,kind,created_at) VALUES(?1,?2,?3,?4);", error };
                if (!restore || !BindText(db, restore.Get(), 1, command.Task.Value(), error) || !BindText(db, restore.Get(), 2, *note, error) ||
                    !BindInt64(db, restore.Get(), 3, noteKind, error) || !BindInt64(db, restore.Get(), 4, noteTime, error) || !StepDone(db, restore.Get(), error)) return result;
            }
            uint64_t sequence{};
            if (!NextAggregateSequence(db, command.Task, sequence, error) || !AppendEvent(db, command.Metadata, command.Task, std::nullopt, sequence, std::nullopt, "TaskBoardUndone", error)) return result;
        }
        auto placement = command; placement.Target = source;
        placement.Anchor = next ? std::optional{ TaskId{ *next } } : std::nullopt;
        placement.Placement = next ? BoardPlacement::Before : BoardPlacement::End;
        if (!PlaceBoardTask(db, placement, source, error)) return result;
        SqliteStatement consumed{ db, "UPDATE task_board_undo SET undone_by=?1 WHERE command_id=?2 AND undone_by IS NULL;", error };
        if (!consumed || !BindText(db, consumed.Get(), 1, command.Metadata.Command.Value(), error) || !BindText(db, consumed.Get(), 2, command.UndoOf->Value(), error) ||
            !StepDone(db, consumed.Get(), error) || sqlite3_changes(db) != 1) return result;
        SqliteStatement event{ db, "INSERT INTO project_event(workspace_id,task_id,kind,version,created_at) VALUES(?1,?2,'board_undo',?3,?4);", error };
        if (!event || !BindText(db, event.Get(), 1, command.Workspace.Value(), error) || !BindText(db, event.Get(), 2, command.Task.Value(), error) ||
            !BindInt64(db, event.Get(), 3, static_cast<int64_t>(result.TaskVersion), error) || !BindInt64(db, event.Get(), 4, command.Metadata.TimestampUnixMilliseconds, error) || !StepDone(db, event.Get(), error)) return result;
        result.EventSequence = sqlite3_last_insert_rowid(db);
        if (!ReadBoardRevision(db, command.Workspace, source, result.ColumnRevision, error)) return result;
        result.Status = TaskStoreCommandStatus::Succeeded; return result;
    }
}
