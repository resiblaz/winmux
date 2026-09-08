// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "SqliteTaskStoreInternal.h"
#include "TaskStoreBoardSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreSqlite.h"
#include "TaskStatusPolicy.h"
#include <limits>
#include <set>

namespace Microsoft::Terminal::TaskCore
{
    TaskBoardCommandResult SqliteTaskStore::ExecuteTaskBoardCommand(const TaskBoardCommand& command)
    {
        TaskBoardCommandResult result;
        result.Task = command.Task;
        auto& error = result.Error;
        const auto fail = [&](TaskStoreCommandStatus status, std::string reason) { result.Status = status; error = std::move(reason); return result; };
        if (!IsReady()) return fail(TaskStoreCommandStatus::NotReady, LastError());
        if (!details::ValidateCommandMetadata(command.Metadata, error) || !IsCanonicalUuid(command.Task.Value()) ||
            !IsCanonicalUuid(command.Workspace.Value()) || command.Effect > TaskBoardEffect::Undo ||
            ((command.Effect == TaskBoardEffect::Undo) != command.UndoOf.has_value()) || (command.UndoOf && !IsCanonicalUuid(command.UndoOf->Value())) ||
            command.Target > TaskStatus::Cancelled || command.Placement > BoardPlacement::End ||
            (command.Anchor && !IsCanonicalUuid(command.Anchor->Value())) ||
            (command.RelatedRun && !IsCanonicalUuid(command.RelatedRun->Value())) ||
            command.Text.size() > MaximumTaskBlockedReasonBytes || command.Text.find('\0') != std::string::npos ||
            (!command.Text.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.Text.data(), static_cast<int>(command.Text.size()), nullptr, 0) <= 0))
            return fail(TaskStoreCommandStatus::InvalidCommand, "board_invalid_request");
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        auto db = _impl->Database;
        details::SqliteTransaction transaction{ db, error };
        if (!transaction) return result;
        std::optional<TaskBoardCommandResult> receipt;
        if (!details::ReadBoardReceipt(db, command, receipt, error)) return result;
        if (receipt) return *receipt;
        std::optional<details::BoardTaskState> state;
        if (!details::LoadBoardTask(db, command.Task, state, error)) return result;
        if (!state) return fail(TaskStoreCommandStatus::NotFound, "task_not_found");
        if (state->Workspace != command.Workspace) return fail(TaskStoreCommandStatus::InvalidCommand, "task_project_mismatch");
        if (state->ArchivedProject) return fail(TaskStoreCommandStatus::InvalidTransition, "project_archived");
        if (command.Effect == TaskBoardEffect::Undo)
        {
            result = details::ApplyBoardUndo(db, command, *state);
            if (!result.Succeeded()) return result;
            if (!details::WriteBoardReceipt(db, command, result, error) || !transaction.Commit(error)) result.Status = TaskStoreCommandStatus::StorageError;
            return result;
        }
        const auto undoable = command.Effect == TaskBoardEffect::Reorder || command.Effect == TaskBoardEffect::MovePlanning;
        details::BoardUndoSnapshot undo;
        if (undoable && !details::CaptureBoardUndo(db, command.Task, undo, error)) return result;
        const auto reorder = command.Effect == TaskBoardEffect::Reorder;
        if (!reorder && state->PendingBoardOperation) return fail(TaskStoreCommandStatus::ActiveIntentExists, "board_operation_pending");
        if (!reorder && state->Task.Version != command.Metadata.ExpectedTaskVersion)
            return fail(TaskStoreCommandStatus::VersionConflict, "task_version_conflict");
        if (!reorder && (command.Metadata.TimestampUnixMilliseconds < state->Task.UpdatedAtUnixMilliseconds ||
                        state->Task.Version >= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())))
            return fail(TaskStoreCommandStatus::InvalidCommand, "task_stale_timestamp_or_version");
        if (!reorder && state->Intent && state->Intent->Stage != TaskIntentStage::Completed)
            return fail(TaskStoreCommandStatus::ActiveIntentExists, "active_intent_conflict");
        result.TaskVersion = state->Task.Version;
        auto target = command.Target;
        auto hold = state->Task.ManualHold;
        bool changed = !reorder;
        bool note{};
        if (command.ExpectedColumnRevision || reorder || command.Anchor)
        {
            uint64_t revision{};
            if (!details::ReadBoardRevision(db, command.Workspace, reorder ? state->Task.Status : target, revision, error)) return result;
            if (!command.ExpectedColumnRevision || revision != command.ExpectedColumnRevision)
                return fail(TaskStoreCommandStatus::VersionConflict, "column_version_conflict");
        }
        if (reorder)
        {
            if (target != state->Task.Status) return fail(TaskStoreCommandStatus::InvalidTransition, "reorder_cannot_change_status");
        }
        else if (command.Effect == TaskBoardEffect::UpdateAcceptance)
        {
            if (state->ActiveRun) return fail(TaskStoreCommandStatus::ActiveRunExists, "active_run_conflict");
            if (state->Task.Status != TaskStatus::InReview) return fail(TaskStoreCommandStatus::InvalidTransition, "review_required");
            target = state->Task.Status;
            if (command.Acceptance.empty() || command.Acceptance.size() > 256) return fail(TaskStoreCommandStatus::InvalidCommand, "acceptance_invalid_patch");
            std::set<uint32_t> positions;
            for (const auto& [position, satisfied] : command.Acceptance)
            {
                if (position >= 256 || !positions.insert(position).second) return fail(TaskStoreCommandStatus::InvalidCommand, "acceptance_invalid_position");
                details::SqliteStatement update{ db, "UPDATE task_acceptance SET satisfied=?1 WHERE task_id=?2 AND position=?3;", error };
                if (!update || !details::BindInt64(db, update.Get(), 1, satisfied, error) || !details::BindText(db, update.Get(), 2, command.Task.Value(), error) ||
                    !details::BindInt64(db, update.Get(), 3, position, error) || !details::StepDone(db, update.Get(), error)) return result;
                if (sqlite3_changes(db) != 1) return fail(TaskStoreCommandStatus::InvalidCommand, "acceptance_missing_position");
            }
        }
        else if (command.Effect == TaskBoardEffect::Start || command.Effect == TaskBoardEffect::Continue)
        {
            if (target != TaskStatus::InProgress || state->ActiveRun || !IsCanonicalUuid(command.NewRun.Value()) ||
                (!command.Text.empty() && !IsCanonicalUuid(command.NewMessage.Value())))
                return fail(TaskStoreCommandStatus::ActiveRunExists, "active_run_or_invalid_start");
            if (command.Effect == TaskBoardEffect::Continue ? (state->Task.Status != TaskStatus::InReview || command.Text.empty()) :
                (state->Task.Status != TaskStatus::Todo && state->Task.Status != TaskStatus::Blocked))
                return fail(TaskStoreCommandStatus::InvalidTransition, "board_start_invalid_status");
            if (!details::StartBoardRun(db, command, *state, error)) return fail(TaskStoreCommandStatus::InvalidTransition, error);
            result.Run = command.NewRun;
            result.Pending = true;
            hold = false;
        }
        else if (command.Effect == TaskBoardEffect::StopThenMove)
        {
            if (state->Task.Status != TaskStatus::InProgress || (target != TaskStatus::Todo && target != TaskStatus::Blocked) ||
                (target == TaskStatus::Blocked && command.Text.empty()))
                return fail(TaskStoreCommandStatus::InvalidTransition, "board_stop_invalid_status");
            if (state->ActiveRun)
            {
                if (!command.RelatedRun || state->ActiveRun->Id != *command.RelatedRun)
                    return fail(TaskStoreCommandStatus::VersionConflict, "active_run_changed");
                if (!IsCanonicalUuid(command.Intent.Value())) return fail(TaskStoreCommandStatus::InvalidCommand, "intent_identity_required");
                if (!details::BeginBoardIntent(db, command, TaskIntentKind::Stop, false, error)) return result;
                details::SqliteStatement operation{ db, "INSERT INTO task_board_operation(operation_id,command_id,task_id,related_run_id,intent_id,source_status,target_status,reason,created_at) VALUES(?1,?1,?2,?3,?4,?5,?6,?7,?8);", error };
                if (!operation || !details::BindText(db, operation.Get(), 1, command.Metadata.Command.Value(), error) ||
                    !details::BindText(db, operation.Get(), 2, command.Task.Value(), error) || !details::BindText(db, operation.Get(), 3, command.RelatedRun->Value(), error) ||
                    !details::BindText(db, operation.Get(), 4, command.Intent.Value(), error) ||
                    !details::BindInt64(db, operation.Get(), 5, static_cast<int64_t>(state->Task.Status), error) ||
                    !details::BindInt64(db, operation.Get(), 6, static_cast<int64_t>(target), error) || !details::BindText(db, operation.Get(), 7, command.Text, error) ||
                    !details::BindInt64(db, operation.Get(), 8, command.Metadata.TimestampUnixMilliseconds, error) || !details::StepDone(db, operation.Get(), error)) return result;
                target = state->Task.Status;
                result.Operation = command.Metadata.Command;
                result.Run = command.RelatedRun;
                result.Pending = true;
            }
            else note = target == TaskStatus::Blocked;
            hold = true;
        }
        else
        {
            if (state->ActiveRun) return fail(TaskStoreCommandStatus::ActiveRunExists, "active_run_conflict");
            TaskStatusTransitionContext context;
            if (!details::HasUnclaimedMessages(db, command.Task, context.HasUnclaimedMessages, error) ||
                !details::HasPendingAutomaticRetry(db, command.Task, context.HasPendingAutomaticRetry, error)) return result;
            const auto decision = EvaluateTaskStatusTransition(state->Task.Status, target, TaskStatusAuthority::User, context);
            if (!decision.Allowed) return fail(TaskStoreCommandStatus::InvalidTransition, "board_transition_rejected");
            switch (command.Effect)
            {
            case TaskBoardEffect::MovePlanning:
                if (target != TaskStatus::Todo && target != TaskStatus::Backlog && target != TaskStatus::Blocked)
                    return fail(TaskStoreCommandStatus::InvalidTransition, "board_planning_target_invalid");
                if (target == TaskStatus::Blocked && command.Text.empty()) return fail(TaskStoreCommandStatus::InvalidCommand, "blocked_reason_required");
                if (state->Task.Status == TaskStatus::Done || state->Task.Status == TaskStatus::Cancelled)
                    return fail(TaskStoreCommandStatus::InvalidTransition, "explicit_reopen_required");
                hold = true; note = target == TaskStatus::Blocked;
                break;
            case TaskBoardEffect::SubmitForReview:
                if (target != TaskStatus::InReview || command.Text.empty()) return fail(TaskStoreCommandStatus::InvalidCommand, "delivery_required");
                note = true;
                break;
            case TaskBoardEffect::ReopenOnly:
                if (target != TaskStatus::Todo || (state->Task.Status != TaskStatus::Done && state->Task.Status != TaskStatus::Cancelled))
                    return fail(TaskStoreCommandStatus::InvalidTransition, "reopen_invalid_status");
                hold = true;
                break;
            case TaskBoardEffect::MarkDone:
                if (target != TaskStatus::Done || state->Task.Status != TaskStatus::InReview || !IsCanonicalUuid(command.Intent.Value()))
                    return fail(TaskStoreCommandStatus::InvalidTransition, "review_required");
                {
                    details::SqliteStatement acceptance{ db, "UPDATE task_acceptance SET satisfied=1 WHERE task_id=?1;", error };
                    if (!acceptance || !details::BindText(db, acceptance.Get(), 1, command.Task.Value(), error) || !details::StepDone(db, acceptance.Get(), error) ||
                        !details::BeginBoardIntent(db, command, TaskIntentKind::Close, true, error)) return result;
                }
                hold = true; result.Pending = true;
                break;
            default: return fail(TaskStoreCommandStatus::InvalidCommand, "board_invalid_effect");
            }
        }
        if (changed)
        {
            if (!details::UpdateBoardTask(db, command.Task, state->Task, target, hold, command.Metadata.TimestampUnixMilliseconds, error)) return result;
            result.TaskVersion = state->Task.Version + 1;
            if (note)
            {
                details::SqliteStatement save{ db, "INSERT INTO task_board_note(task_id,text,kind,created_at) VALUES(?1,?2,?3,?4) ON CONFLICT(task_id) DO UPDATE SET text=excluded.text,kind=excluded.kind,created_at=excluded.created_at;", error };
                if (!save || !details::BindText(db, save.Get(), 1, command.Task.Value(), error) || !details::BindText(db, save.Get(), 2, command.Text, error) ||
                    !details::BindInt64(db, save.Get(), 3, target == TaskStatus::Blocked, error) ||
                    !details::BindInt64(db, save.Get(), 4, command.Metadata.TimestampUnixMilliseconds, error) || !details::StepDone(db, save.Get(), error)) return result;
            }
            uint64_t sequence{};
            if (!details::NextAggregateSequence(db, command.Task, sequence, error) ||
                !details::AppendEvent(db, command.Metadata, command.Task, result.Run, sequence,
                                     result.Run && !result.Operation ? std::optional<uint64_t>{ 1 } : std::nullopt, "TaskBoardAction", error)) return result;
        }
        if (reorder || (command.Anchor && !result.Operation))
        {
            if (command.Anchor && *command.Anchor == command.Task) return fail(TaskStoreCommandStatus::InvalidCommand, "board_self_anchor");
            if (!details::PlaceBoardTask(db, command, target, error)) return fail(TaskStoreCommandStatus::VersionConflict, error);
        }
        details::SqliteStatement event{ db, "INSERT INTO project_event(workspace_id,task_id,kind,version,created_at) VALUES(?1,?2,'board_action',?3,?4);", error };
        if (!event || !details::BindText(db, event.Get(), 1, command.Workspace.Value(), error) || !details::BindText(db, event.Get(), 2, command.Task.Value(), error) ||
            !details::BindInt64(db, event.Get(), 3, static_cast<int64_t>(result.TaskVersion), error) ||
            !details::BindInt64(db, event.Get(), 4, command.Metadata.TimestampUnixMilliseconds, error) || !details::StepDone(db, event.Get(), error)) return result;
        result.EventSequence = sqlite3_last_insert_rowid(db);
        if (!details::ReadBoardRevision(db, command.Workspace, target, result.ColumnRevision, error) ||
            (undoable && !details::SaveBoardUndo(db, command, *state, undo, result, error)) ||
            !details::WriteBoardReceipt(db, command, result, error) || !transaction.Commit(error)) return result;
        result.Status = TaskStoreCommandStatus::Succeeded;
        return result;
    }
}
