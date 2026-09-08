// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskPage.h"
#include "../Agents/AgentSelectionControl.h"

using namespace winrt::Windows::UI::Xaml;

namespace winrt::TerminalApp::implementation
{
    void TaskPage::_CapturePageState()
    {
        if (!_pageState) return;
        auto& state = *_pageState;
        state.Search = TaskSearchBox().Text(); state.StatusFilter = StatusFilter().SelectedIndex();
        state.AllProjects = winrt::unbox_value_or<bool>(AllWorkspacesScope().IsChecked(), false);
        state.Notice = TaskOperationStatus().Text();
        state.SelectedTask = _viewModel.SelectedTask() ? std::wstring{ _viewModel.SelectedTask().Id() } : std::wstring{};
        auto& draft = state.Draft;
        if (AgentManagerHost().Visibility() != Visibility::Visible) draft.Visible = DraftLayer().Visibility() == Visibility::Visible;
        draft.Title = DraftTitle().Text(); draft.Description = DraftDescription().Text(); draft.Acceptance = DraftAcceptance().Text();
        draft.ExecutionMode = DraftExecutionMode().SelectedIndex();
        draft.Agent = winrt::get_self<implementation::AgentSelectionControl>(DraftAgent())->CaptureState();
        draft.Saving = _draftSaving; draft.OutcomeUnknown = _draftOutcomeUnknown; draft.Pending = _pendingCreation; draft.Error = DraftError().Text();
    }

    void TaskPage::_RestorePageState()
    {
        if (!_pageState) return;
        const auto restore = wil::scope_exit([&] { _restoringPageState = false; });
        _restoringPageState = true;
        auto& state = *_pageState;
        TaskOperationStatus().Text(state.Notice);
        RetryBoardActionButton().Visibility(state.RetryBoardTask.empty() ? Visibility::Collapsed : Visibility::Visible);
        UndoBoardActionButton().Visibility(_actions && _actions->CanUndo(state.UndoProjectId) ? Visibility::Visible : Visibility::Collapsed);
        _SetBoardMode(state.Board);
        TaskSearchBox().Text(state.Search); StatusFilter().SelectedIndex(state.StatusFilter);
        CurrentWorkspaceScope().IsChecked(!state.AllProjects); AllWorkspacesScope().IsChecked(state.AllProjects);
        _ViewModelImpl()->SetSearchText(winrt::hstring{ state.Search }); _ViewModelImpl()->SetStatusFilter(state.StatusFilter); _ViewModelImpl()->SetShowAllWorkspaces(state.AllProjects);
        _ViewModelImpl()->SelectCreatedTask(state.SelectedTask);
        auto& draft = state.Draft;
        _draftSaving = draft.Saving; _draftOutcomeUnknown = draft.OutcomeUnknown; _pendingCreation = draft.Pending;
        DraftTitle().Text(draft.Title); DraftDescription().Text(draft.Description); DraftAcceptance().Text(draft.Acceptance);
        DraftExecutionMode().SelectedIndex(draft.ExecutionMode); DraftError().Text(draft.Error);
        DraftLayer().Visibility(draft.Visible ? Visibility::Visible : Visibility::Collapsed);
        if (draft.Visible)
        {
            const auto selector = winrt::get_self<implementation::AgentSelectionControl>(DraftAgent());
            if (draft.Agent) selector->RestoreState(_source ? _source->AgentCatalog() : nullptr, til::u16u8(_currentWorkspaceId), *draft.Agent);
            else selector->Configure(_source ? _source->AgentCatalog() : nullptr, til::u16u8(_currentWorkspaceId));
        }
        _SyncDraftWorkspace(); _UpdateDraftEnabled(); _SyncSelection();
    }
}
