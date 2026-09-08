// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "TaskProjection.h"
#include "../Agents/AgentSelectionState.h"
#include <memory>
#include <unordered_map>

namespace winrt::TerminalApp::implementation::tasks
{
    struct TaskDraftSession
    {
        std::wstring Title;
        std::wstring Description;
        std::wstring Acceptance;
        std::wstring SourceRoot;
        uint64_t ProjectVersion{};
        std::optional<agents::AgentSelectionState> Agent;
        int32_t ExecutionMode{};
        bool Visible{};
        bool Saving{};
        bool OutcomeUnknown{};
        std::wstring Error;
        std::optional<TaskCreationAttempt> Pending;
    };

    struct TaskPageState
    {
        std::wstring Search;
        std::wstring SelectedTask;
        std::wstring Notice;
        std::wstring RetryBoardTask;
        std::string UndoProjectId;
        bool AllProjects{};
        bool Board{};
        int32_t StatusFilter{};
        TaskDraftSession Draft;
    };

    class TaskPageStateStore
    {
    public:
        bool Contains(const std::wstring& id) const { return _states.contains(id); }
        std::shared_ptr<TaskPageState> ForProject(const std::wstring& id)
        {
            auto& state = _states[id];
            if (!state) state = std::make_shared<TaskPageState>();
            return state;
        }
    private:
        std::unordered_map<std::wstring, std::shared_ptr<TaskPageState>> _states;
    };
}
