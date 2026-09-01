// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProjectionSource.h"

#include "RuntimeCrypto.h"

namespace winrt::TerminalApp::implementation::tasks
{
    using namespace ::Microsoft::Terminal::TaskRuntime;
    using namespace ::Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        std::optional<std::string> _uuid()
        {
            return NewRuntimeUuid();
        }

        std::wstring _resource(const wchar_t* key)
        {
            const auto value = RS_(key);
            return std::wstring{ std::wstring_view{ value } };
        }

        TaskProjection _optimistic(
            const TaskDraft& draft,
            const std::string_view id,
            const bool startImmediately)
        {
            TaskProjection result;
            result.Id = til::u8u16(id);
            result.WorkspaceKey = draft.WorkspaceId;
            result.Workspace = draft.Workspace.empty() ? draft.WorkspaceId : draft.Workspace;
            result.Title = draft.Title;
            result.Description = draft.Description;
            result.Status = TaskStatus::Todo;
            result.TerminalState = TaskTerminalState::NotCreated;
            result.Priority = TaskPriority::Medium;
            result.UpdatedLabel = _resource(L"TaskPage_UpdatedJustNow");
            result.ExecutionMode = _resource(draft.UseWorktree ? L"TaskPage_WorktreeExecutionMode" : L"TaskPage_DirectExecutionMode");
            result.WorkingDirectory = draft.SourceRoot;
            result.Worktree = draft.UseWorktree ? _resource(L"TaskPage_WorktreePreparing") : _resource(L"TaskPage_NotApplicable");
            result.Branch = draft.UseWorktree ? _resource(L"TaskPage_WorktreePreparing") : _resource(L"TaskPage_CurrentBranch");
            result.Version = 1;
            result.SupportsTerminalSurface = true;
            result.HasActiveRun = startImmediately;
            result.ActiveRunSummary = startImmediately ? _resource(L"TaskPage_NewTaskRunQueued") : std::wstring{};
            for (const auto& item : draft.Acceptance)
            {
                result.Acceptance.push_back({ item, false });
            }
            return result;
        }
    }

    TaskProjection RuntimeTaskProjectionSource::CreateTask(
        const TaskDraft& draft,
        const bool startImmediately)
    {
        const auto taskId = _uuid();
        const auto createId = _uuid();
        if (!taskId || !createId || draft.WorkspaceId.empty() || draft.SourceRoot.empty())
            return {};
        TaskUiCommandRequest create;
        create.CommandId = *createId;
        create.Operation = TaskUiCommandOperation::Create;
        create.TaskId = *taskId;
        create.Draft = TaskUiCreateDraft{};
        create.Draft->WorkspaceId = til::u16u8(draft.WorkspaceId);
        create.Draft->Title = til::u16u8(draft.Title);
        create.Draft->Description = til::u16u8(draft.Description);
        for (const auto& item : draft.Acceptance)
        {
            create.Draft->Acceptance.emplace_back(til::u16u8(item));
        }
        create.Draft->ExecutionMode = draft.UseWorktree ? TaskProjectionExecutionMode::Worktree : TaskProjectionExecutionMode::Direct;
        create.Draft->SourceRoot = til::u16u8(draft.SourceRoot);
        create.Draft->RelativeWorkingDirectory = til::u16u8(draft.RelativeWorkingDirectory);
        create.Draft->Model = til::u16u8(draft.Model);
        create.Draft->Instructions = til::u16u8(draft.Instructions);
        auto projection = _optimistic(draft, *taskId, startImmediately);
        if (!_enqueue({ WorkKind::Command, create, std::nullopt }))
            return {};
        _coordinator.UpsertOptimistic(projection);
        if (startImmediately)
        {
            const auto startId = _uuid();
            if (startId)
            {
                TaskUiCommandRequest start;
                start.CommandId = *startId;
                start.Operation = TaskUiCommandOperation::Start;
                start.TaskId = *taskId;
                start.ExpectedTaskVersion = 1;
                static_cast<void>(_enqueue({ WorkKind::Command, start, std::nullopt }));
            }
        }
        _notify();
        return projection;
    }

    std::optional<TaskProjection> RuntimeTaskProjectionSource::Execute(
        const std::wstring& taskId,
        const TaskCommand command)
    {
        const auto snapshot = Snapshot();
        const auto task = std::ranges::find(snapshot, taskId, &TaskProjection::Id);
        if (task == snapshot.end())
            return std::nullopt;
        TaskUiCommandOperation operation;
        switch (command)
        {
        case TaskCommand::Start:
            operation = TaskUiCommandOperation::Start;
            break;
        case TaskCommand::Stop:
            operation = TaskUiCommandOperation::Stop;
            break;
        case TaskCommand::OpenTerminal:
            operation = TaskUiCommandOperation::OpenTerminal;
            break;
        case TaskCommand::CloseTerminal:
            operation = TaskUiCommandOperation::Close;
            break;
        case TaskCommand::MarkDone:
            operation = TaskUiCommandOperation::MarkDone;
            break;
        case TaskCommand::Reopen:
            operation = TaskUiCommandOperation::Reopen;
            break;
        case TaskCommand::Retry:
            operation = TaskUiCommandOperation::Retry;
            break;
        case TaskCommand::Cancel:
            operation = TaskUiCommandOperation::Cancel;
            break;
        case TaskCommand::FinalizeWorktree:
            operation = TaskUiCommandOperation::FinalizeWorktree;
            break;
        case TaskCommand::Delete:
            operation = TaskUiCommandOperation::Delete;
            break;
        case TaskCommand::RestartProvider:
            operation = TaskUiCommandOperation::RestartProvider;
            break;
        default:
            return *task;
        }
        const auto commandId = _uuid();
        if (!commandId)
            return std::nullopt;
        TaskUiCommandRequest request;
        request.CommandId = *commandId;
        request.Operation = operation;
        request.TaskId = til::u16u8(task->Id);
        request.ExpectedTaskVersion = task->Version;
        if (command == TaskCommand::Retry)
        {
            if (task->LatestTerminalRunId.empty())
                return std::nullopt;
            request.RelatedRunId = til::u16u8(task->LatestTerminalRunId);
        }
        return _enqueue({ WorkKind::Command, request, std::nullopt }) ? std::optional<TaskProjection>{ *task } : std::nullopt;
    }

    bool RuntimeTaskProjectionSource::QueueMessage(
        const std::wstring& taskId,
        const std::wstring& message)
    {
        if (message.empty())
            return false;
        const auto snapshot = Snapshot();
        const auto task = std::ranges::find(snapshot, taskId, &TaskProjection::Id);
        const auto commandId = _uuid();
        if (task == snapshot.end() || !commandId)
            return false;
        TaskUiCommandRequest request;
        request.CommandId = *commandId;
        request.Operation = TaskUiCommandOperation::QueueMessage;
        request.TaskId = til::u16u8(taskId);
        request.ExpectedTaskVersion = task->Version;
        request.Text = til::u16u8(message);
        return _enqueue({ WorkKind::Command, std::move(request), std::nullopt });
    }

    bool RuntimeTaskProjectionSource::ReviseMessage(
        const std::wstring& taskId,
        const std::wstring& messageId,
        const uint64_t revision,
        const std::optional<std::wstring>& replacement)
    {
        const auto snapshot = Snapshot();
        const auto task = std::ranges::find(snapshot, taskId, &TaskProjection::Id);
        const auto commandId = _uuid();
        if (task == snapshot.end() || !commandId || messageId.empty() || revision == 0)
            return false;
        TaskUiCommandRequest request;
        request.CommandId = *commandId;
        request.Operation = replacement ? TaskUiCommandOperation::ReviseMessage : TaskUiCommandOperation::RemoveMessage;
        request.TaskId = til::u16u8(taskId);
        request.ExpectedTaskVersion = task->Version;
        request.MessageId = til::u16u8(messageId);
        request.ExpectedMessageRevision = revision;
        if (replacement)
        {
            request.Text = til::u16u8(*replacement);
        }
        return _enqueue({ WorkKind::Command, std::move(request), std::nullopt });
    }
}
