// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "ITaskProjectionSource.h"
#include "../Projects/IWorkspaceSource.h"
#include <map>

namespace winrt::TerminalApp::implementation::tasks
{
    struct BoardActionResolution
    {
        bool Allowed{};
        projects::protocol::TaskBoardOperation Operation{ projects::protocol::TaskBoardOperation::MovePlanning };
        bool Confirm{};
        bool RequiresText{};
        const wchar_t* MessageKey{ L"TaskBoard_InvalidMove" };
    };
    [[nodiscard]] BoardActionResolution ResolveBoardAction(const TaskProjection& task, TaskStatus target) noexcept;

    class TaskActionController final : public std::enable_shared_from_this<TaskActionController>
    {
    public:
        explicit TaskActionController(std::shared_ptr<ITaskProjectionSource> source) : _source{ std::move(source) } {}
        using ChangedHandler = std::function<void(const projects::protocol::TaskBoardCommandRequest&, std::wstring, bool)>;
        void SetChangedHandler(ChangedHandler handler) { _changed = std::move(handler); }
        bool Pending(std::wstring_view taskId) const;
        void Execute(projects::protocol::TaskBoardCommandRequest request);
        void Retry(std::wstring_view taskId);
        bool CanUndo(std::string_view projectId) const;
        void Undo(std::string_view projectId);
        void PollOperations();
    private:
        struct PendingAction
        {
            projects::protocol::TaskBoardCommandRequest Request;
            bool Sending{};
            bool Unknown{};
            std::string OperationId;
        };
        std::shared_ptr<ITaskProjectionSource> _source;
        std::map<std::wstring, PendingAction> _pending;
        std::map<std::string, projects::protocol::TaskBoardCommandRequest> _undo;
        ChangedHandler _changed;
        void _Send(std::wstring taskId);
    };
}
