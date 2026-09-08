// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "SqliteTaskStoreInternal.h"
#include "TaskStoreBoardSqlite.h"
#include "TaskStoreSqlite.h"
#include <objbase.h>

#pragma comment(lib, "ole32.lib")

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        std::optional<TaskEventId> eventId()
        {
            GUID guid;
            wchar_t text[40]{};
            if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(guid, text, 40) != 39) return std::nullopt;
            std::string value;
            for (size_t i = 1; i <= 36; ++i)
            {
                auto c = static_cast<char>(text[i]);
                if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
                value.push_back(c);
            }
            return TaskEventId{ std::move(value) };
        }
    }

    void SqliteTaskStore::ReconcileTaskBoardOperations()
    {
        if (!IsReady()) return;
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        auto db = _impl->Database;
        std::string error;
        struct Pending { CommandId Id; TaskId Task; TaskRunId Run; TaskIntentId Intent; TaskStatus Source; TaskStatus Target; std::string Text; int64_t Timestamp; };
        std::vector<Pending> work;
        {
            details::SqliteStatement list{ db, "SELECT operation_id,task_id,related_run_id,intent_id,source_status,target_status,reason,created_at FROM task_board_operation WHERE stage=0 ORDER BY created_at,operation_id LIMIT 64;", error };
            if (!list) return;
            int step;
            while ((step = sqlite3_step(list.Get())) == SQLITE_ROW)
                work.push_back({ CommandId{ details::ColumnText(list.Get(), 0) }, TaskId{ details::ColumnText(list.Get(), 1) },
                                 TaskRunId{ details::ColumnText(list.Get(), 2) }, TaskIntentId{ details::ColumnText(list.Get(), 3) },
                                 static_cast<TaskStatus>(sqlite3_column_int(list.Get(), 4)), static_cast<TaskStatus>(sqlite3_column_int(list.Get(), 5)),
                                 details::ColumnText(list.Get(), 6), sqlite3_column_int64(list.Get(), 7) });
            if (step != SQLITE_DONE) return;
        }
        for (const auto& operation : work)
        {
            error.clear();
            details::SqliteTransaction transaction{ db, error };
            if (!transaction) return;
            std::optional<details::BoardTaskState> task;
            if (!details::LoadBoardTask(db, operation.Task, task, error) || !task) continue;
            std::string failure;
            if (!task->Intent || task->Intent->Id != operation.Intent) failure = "operation_superseded";
            else if (task->Intent->Stage == TaskIntentStage::Failed) failure = "stop_failed";
            else if (task->Intent->Stage != TaskIntentStage::Completed || task->ActiveRun) continue;
            else if (task->Task.Status != operation.Source && task->Task.Status != operation.Target && task->Task.Status != TaskStatus::Todo)
                failure = "operation_superseded";
            // While the owned Stop is pending, user status changes and new Runs are guarded.
            // Todo here is the existing SystemRecovery result for this interrupted Run.
            if (!details::UpdateBoardTask(db, operation.Task, task->Task, failure.empty() ? operation.Target : task->Task.Status,
                                         true, task->Task.UpdatedAtUnixMilliseconds, error)) continue;
            if (failure.empty() && task->Task.Status != operation.Target)
            {
                if (operation.Target == TaskStatus::Blocked)
                {
                    details::SqliteStatement note{ db, "INSERT INTO task_board_note(task_id,text,kind,created_at) VALUES(?1,?2,1,?3) ON CONFLICT(task_id) DO UPDATE SET text=excluded.text,kind=1,created_at=excluded.created_at;", error };
                    if (!note || !details::BindText(db, note.Get(), 1, operation.Task.Value(), error) ||
                        !details::BindText(db, note.Get(), 2, operation.Text, error) ||
                        !details::BindInt64(db, note.Get(), 3, task->Task.UpdatedAtUnixMilliseconds, error) || !details::StepDone(db, note.Get(), error)) continue;
                }
            }
            const auto id = eventId();
            if (!id) continue;
            TaskCommandMetadata metadata;
            metadata.Command = operation.Id; metadata.Event = *id;
            metadata.TimestampUnixMilliseconds = (std::max)(operation.Timestamp, task->Task.UpdatedAtUnixMilliseconds);
            metadata.EventPayloadJson = "{\"operationId\":\"" + operation.Id.Value() + "\",\"reasonCode\":\"" + failure + "\"}";
            uint64_t sequence{};
            if (!details::NextAggregateSequence(db, operation.Task, sequence, error) ||
                !details::AppendEvent(db, metadata, operation.Task, std::nullopt, sequence, std::nullopt,
                                     failure.empty() ? "TaskBoardOperationCompleted" : "TaskBoardOperationFailed", error)) continue;
            details::SqliteStatement finish{ db, "UPDATE task_board_operation SET stage=?1,reason=?2 WHERE operation_id=?3 AND stage=0;", error };
            if (!finish || !details::BindInt64(db, finish.Get(), 1, failure.empty() ? 1 : 2, error) ||
                !details::BindText(db, finish.Get(), 2, failure, error) || !details::BindText(db, finish.Get(), 3, operation.Id.Value(), error) ||
                !details::StepDone(db, finish.Get(), error)) continue;
            details::SqliteStatement event{ db, "INSERT INTO project_event(workspace_id,task_id,kind,version,created_at) VALUES(?1,?2,'board_operation',?3,?4);", error };
            if (!event || !details::BindText(db, event.Get(), 1, task->Workspace.Value(), error) ||
                !details::BindText(db, event.Get(), 2, operation.Task.Value(), error) ||
                !details::BindInt64(db, event.Get(), 3, static_cast<int64_t>(task->Task.Version + 1), error) ||
                !details::BindInt64(db, event.Get(), 4, metadata.TimestampUnixMilliseconds, error) || !details::StepDone(db, event.Get(), error)) continue;
            if (!transaction.Commit(error)) return;
        }
    }

    TaskBoardCommandResult SqliteTaskStore::QueryTaskBoardOperation(const CommandId& operation) const
    {
        TaskBoardCommandResult result;
        if (!IsReady()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = LastError(); return result; }
        if (!IsCanonicalUuid(operation.Value())) { result.Status = TaskStoreCommandStatus::InvalidCommand; return result; }
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        details::SqliteStatement row{ _impl->Database, "SELECT o.task_id,o.related_run_id,o.stage,o.reason,t.version FROM task_board_operation o JOIN task t ON t.id=o.task_id WHERE o.operation_id=?1;", result.Error };
        if (!row || !details::BindText(_impl->Database, row.Get(), 1, operation.Value(), result.Error)) return result;
        const auto step = sqlite3_step(row.Get());
        if (step == SQLITE_DONE) { result.Status = TaskStoreCommandStatus::NotFound; result.Error = "board_operation_not_found"; return result; }
        if (step != SQLITE_ROW) return result;
        result.Operation = operation; result.Task = TaskId{ details::ColumnText(row.Get(), 0) };
        result.Run = TaskRunId{ details::ColumnText(row.Get(), 1) }; result.Pending = sqlite3_column_int(row.Get(), 2) == 0;
        result.TaskVersion = sqlite3_column_int64(row.Get(), 4);
        if (sqlite3_column_int(row.Get(), 2) == 2) { result.Status = TaskStoreCommandStatus::VersionConflict; result.Error = details::ColumnText(row.Get(), 3); }
        else result.Status = TaskStoreCommandStatus::Succeeded;
        return result;
    }
}
