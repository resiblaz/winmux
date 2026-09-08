// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskActionController.h"
#include "RuntimeCrypto.h"

namespace winrt::TerminalApp::implementation::tasks
{
    using namespace projects;
    using namespace projects::protocol;
    BoardActionResolution ResolveBoardAction(const TaskProjection& task, TaskStatus target) noexcept
    {
        using Operation = TaskBoardOperation;
        if (task.ArchivedProject) return { false, {}, false, false, L"TaskBoard_ProjectArchived" };
        if (task.Status == target) return { true, Operation::Reorder, false, false, L"TaskBoard_Reorder" };
        if (task.HasPendingIntent) return { false, {}, false, false, L"TaskBoard_PendingOperation" };
        if (task.HasActiveRun)
        {
            if (task.Status == TaskStatus::InProgress && (target == TaskStatus::Todo || target == TaskStatus::Blocked))
                return { true, Operation::StopThenMove, true, target == TaskStatus::Blocked, L"TaskBoard_StopThenMove" };
            return { false, {}, false, false, L"TaskBoard_ActiveRun" };
        }
        if (target == TaskStatus::Todo && (task.Status == TaskStatus::Done || task.Status == TaskStatus::Cancelled))
            return { true, Operation::ReopenOnly, false, false, L"TaskBoard_ReopenOnly" };
        if (target == TaskStatus::InProgress)
        {
            if (task.Status == TaskStatus::Todo || task.Status == TaskStatus::Blocked)
                return { true, Operation::Start, false, false, L"TaskBoard_Start" };
            if (task.Status == TaskStatus::InReview)
                return { true, Operation::Continue, true, true, L"TaskBoard_RequestChanges" };
        }
        if (target == TaskStatus::InReview && task.Status == TaskStatus::InProgress)
            return { true, Operation::SubmitForReview, true, true, L"TaskBoard_SubmitReview" };
        if (target == TaskStatus::Done && task.Status == TaskStatus::InReview)
            return task.HasPendingMessages ? BoardActionResolution{ false, {}, false, false, L"TaskBoard_PendingMessages" } :
                                            BoardActionResolution{ true, Operation::MarkDone, true, false, L"TaskBoard_ConfirmDone" };
        if ((task.Status == TaskStatus::Backlog && target == TaskStatus::Todo) ||
            (task.Status == TaskStatus::Todo && target == TaskStatus::Backlog) ||
            (task.Status == TaskStatus::Blocked && target == TaskStatus::Todo) ||
            (task.Status == TaskStatus::InReview && target == TaskStatus::Todo) ||
            (task.Status == TaskStatus::InProgress && target == TaskStatus::Todo))
            return { true, Operation::MovePlanning, false, false, L"TaskBoard_MovePlanning" };
        if (target == TaskStatus::Blocked && task.Status == TaskStatus::InProgress)
            return { true, Operation::MovePlanning, true, true, L"TaskBoard_BlockReason" };
        return {};
    }

    bool TaskActionController::Pending(std::wstring_view taskId) const
    { return _pending.contains(std::wstring{ taskId }); }
    bool TaskActionController::CanUndo(std::string_view projectId) const { return _undo.contains(std::string{ projectId }); }
    void TaskActionController::Undo(std::string_view projectId)
    {
        const auto found = _undo.find(std::string{ projectId }); if (found == _undo.end()) return;
        const auto id = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid(); if (!id) return;
        auto request = found->second; request.CommandId = *id; Execute(std::move(request));
    }

    void TaskActionController::Execute(TaskBoardCommandRequest request)
    {
        const auto id = til::u8u16(request.TaskId);
        if (Pending(id)) return;
        _pending.emplace(id, PendingAction{ std::move(request) }); _Send(id);
    }
    void TaskActionController::Retry(std::wstring_view taskId)
    {
        const auto found = _pending.find(std::wstring{ taskId });
        if (found != _pending.end() && found->second.Unknown && !found->second.Sending) _Send(found->first);
    }
    void TaskActionController::_Send(std::wstring taskId)
    {
        const auto source = _source ? _source->WorkspaceSource() : nullptr;
        const auto found = _pending.find(taskId);
        if (!source || found == _pending.end() || found->second.Sending) return;
        found->second.Sending = true;
        if (_changed) _changed(found->second.Request, std::wstring{ RS_(L"TaskBoard_Submitting") }, false);
        const auto weak = weak_from_this();
        source->ExecuteBoard(found->second.Request, [weak, taskId](WorkspaceReply<TaskBoardCommandResult> reply) {
            const auto self = weak.lock(); if (!self) return;
            const auto pending = self->_pending.find(taskId); if (pending == self->_pending.end()) return;
            pending->second.Sending = false; pending->second.Unknown = reply.OutcomeUnknown;
            const auto request = pending->second.Request;
            if (reply.Succeeded && (request.Operation == TaskBoardOperation::Reorder || request.Operation == TaskBoardOperation::MovePlanning))
            {
                auto undo = request; undo.CommandId.clear(); undo.UndoOfCommandId = request.CommandId; undo.Operation = TaskBoardOperation::Undo;
                undo.ExpectedTaskVersion = reply.Value.TaskVersion; undo.ExpectedColumnRevision = 0;
                undo.AnchorId.reset(); undo.RelatedRunId.reset(); undo.Placement = TaskBoardPlacement::End; undo.Text.clear(); undo.Acceptance.clear();
                self->_undo[request.ProjectId] = std::move(undo);
            }
            else if (request.Operation == TaskBoardOperation::Undo && (reply.Succeeded || reply.ErrorCode == ProtocolRpcErrorCode::Conflict || reply.ErrorCode == ProtocolRpcErrorCode::NotFound))
            {
                const auto undo = self->_undo.find(request.ProjectId);
                if (undo != self->_undo.end() && undo->second.UndoOfCommandId == request.UndoOfCommandId) self->_undo.erase(undo);
            }
            if (reply.Succeeded && reply.Value.OperationId && reply.Value.Pending)
                pending->second.OperationId = *reply.Value.OperationId;
            else if (!reply.OutcomeUnknown) self->_pending.erase(pending);
            self->_source->RequestRefresh();
            if (self->_changed)
                self->_changed(request, reply.Succeeded ? std::wstring{ RS_(request.Operation == TaskBoardOperation::Undo ? L"TaskBoard_Undone" :
                    reply.Value.OperationId && reply.Value.Pending ? L"TaskBoard_WaitingForStop" : L"TaskBoard_Applied") } :
                    reply.Error.starts_with("board_undo_") ? std::wstring{ RS_(L"TaskBoard_UndoConflict") } : til::u8u16(reply.Error), reply.OutcomeUnknown);
        });
    }
    void TaskActionController::PollOperations()
    {
        const auto source = _source ? _source->WorkspaceSource() : nullptr; if (!source) return;
        std::vector<std::pair<std::wstring, std::string>> operations;
        for (auto& [task, operation] : _pending)
            if (!operation.OperationId.empty() && !operation.Sending)
            { operation.Sending = true; operations.emplace_back(task, operation.OperationId); }
        const auto weak = weak_from_this();
        for (const auto& [task, operation] : operations)
            source->QueryOperation({ operation }, [weak, task](WorkspaceReply<TaskBoardCommandResult> reply) {
                const auto self = weak.lock(); if (!self) return;
                const auto found = self->_pending.find(task); if (found == self->_pending.end()) return;
                found->second.Sending = false;
                if ((reply.Succeeded && !reply.Value.Pending) || reply.ErrorCode == ProtocolRpcErrorCode::Conflict || reply.ErrorCode == ProtocolRpcErrorCode::NotFound)
                {
                    const auto request = found->second.Request;
                    self->_pending.erase(found); self->_source->RequestRefresh();
                    if (self->_changed) self->_changed(request, reply.Succeeded ? std::wstring{ RS_(L"TaskBoard_Applied") } : til::u8u16(reply.Error), false);
                }
            });
    }
}
