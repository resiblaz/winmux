// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskPage.h"
#include "TaskDetailsPane.h"
#include "../Agents/AgentSelectionControl.h"
#include "../Agents/AgentCatalogPage.h"

#include "TaskPage.g.cpp"

#include <cwctype>

using namespace winrt;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        std::vector<std::wstring> _lines(const winrt::hstring& value)
        {
            std::vector<std::wstring> result;
            std::wstring_view remaining{ value };
            while (!remaining.empty())
            {
                const auto newline = remaining.find(L'\n');
                auto line = remaining.substr(0, newline);
                while (!line.empty() && std::iswspace(line.front()))
                {
                    line.remove_prefix(1);
                }
                while (!line.empty() && std::iswspace(line.back()))
                {
                    line.remove_suffix(1);
                }
                if (!line.empty())
                {
                    result.emplace_back(line);
                }
                if (newline == std::wstring_view::npos)
                {
                    break;
                }
                remaining.remove_prefix(newline + 1);
            }
            return result;
        }
    }

    TaskPage::TaskPage() :
        _viewModel{ winrt::make<implementation::TaskPageViewModel>() }
    {
        _pageState = _pageStates.ForProject(_currentWorkspaceId);
        InitializeComponent();
        _InitializeBoard();
        const auto weakThis = get_weak();
        winrt::get_self<implementation::AgentSelectionControl>(DraftAgent())->SetChangedHandler([weakThis] {
            if (const auto self = weakThis.get()) self->_UpdateDraftEnabled();
        });
        const auto details = winrt::get_self<implementation::TaskDetailsPane>(DetailsPane());
        details->SetCommandHandler([weakThis](const auto command) {
            if (const auto self = weakThis.get())
            {
                self->_Execute(command);
            }
        });
        details->SetMessageHandler([weakThis](const winrt::hstring& message) {
            if (const auto self = weakThis.get())
            {
                return self->_ViewModelImpl()->QueueSelectedMessage(message);
            }
            return false;
        });
        details->SetMessageMutationHandler([weakThis](
                                               const uint32_t index,
                                               const std::optional<std::wstring>& replacement) {
            if (const auto self = weakThis.get())
            {
                const auto selected = self->_viewModel.SelectedTask();
                if (!selected)
                    return false;
                const auto identity = winrt::get_self<implementation::TaskItemViewModel>(selected)->QueuedMessageIdentityAt(index);
                return identity && self->_ViewModelImpl()->ReviseSelectedMessage(
                                       identity->first, identity->second, replacement);
            }
            return false;
        });
        _SyncSelection();
        _restoringPageState = false;
    }

    TerminalApp::TaskPageViewModel TaskPage::ViewModel() const noexcept
    {
        return _viewModel;
    }

    void TaskPage::AttachProjectionSource(std::shared_ptr<tasks::ITaskProjectionSource> source)
    {
        PrototypeSourceInfo().Visibility(source && source->IsPrototype() ? Visibility::Visible : Visibility::Collapsed);
        _source = std::move(source);
        if (_source)
        {
            _source->SetSurfaceHandler(_surfaceHandler);
        }
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->AttachSource(_source); });
        _actions = std::make_shared<tasks::TaskActionController>(_source);
        const auto weak = get_weak();
        _actions->SetChangedHandler([weak](const auto& request, std::wstring message, bool unknown) {
            if (const auto self = weak.get())
            {
                const auto owner = self->_pageStates.ForProject(til::u8u16(request.ProjectId));
                owner->Notice = message; owner->RetryBoardTask = unknown ? til::u8u16(request.TaskId) : std::wstring{};
                owner->UndoProjectId = request.ProjectId;
                if (owner == self->_pageState || self->_pageState->AllProjects)
                {
                    self->TaskOperationStatus().Text(message);
                    self->_pageState->RetryBoardTask = owner->RetryBoardTask;
                    self->_pageState->UndoProjectId = request.ProjectId;
                    self->UndoBoardActionButton().Visibility(!unknown && self->_actions->CanUndo(request.ProjectId) ? Visibility::Visible : Visibility::Collapsed);
                    self->RetryBoardActionButton().Visibility(unknown ? Visibility::Visible : Visibility::Collapsed);
                }
                self->_UpdateBoard();
            }
        });
        _UpdateDraftEnabled();
    }

    void TaskPage::SetSurfaceHandler(tasks::ITaskProjectionSource::SurfaceHandler handler)
    {
        _surfaceHandler = std::move(handler);
        if (_source)
        {
            _source->SetSurfaceHandler(_surfaceHandler);
        }
    }

    void TaskPage::SetCurrentWorkspace(const winrt::guid& workspaceId,
                                       const winrt::hstring& repositoryRoot,
                                       const winrt::hstring& workingDirectory)
    {
        auto id = std::wstring{ winrt::to_hstring(workspaceId) };
        if (id.size() == 38 && id.front() == L'{' && id.back() == L'}')
        {
            id = id.substr(1, 36);
        }
        std::ranges::transform(id, id.begin(), [](const wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
        const auto changing = _currentWorkspaceId != id;
        if (changing) _CapturePageState();
        _currentWorkspaceId = id;
        _currentRepositoryRoot = repositoryRoot;
        _currentWorkingDirectory = workingDirectory;
        _currentWorkspaceKey = _source ?
                                   _source->ResolveWorkspaceKey(id, std::wstring_view{ repositoryRoot }, std::wstring_view{ workingDirectory }) :
                                   id;
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetCurrentWorkspace(winrt::hstring{ _currentWorkspaceKey }); });
        if (changing)
        {
            ++_draftGeneration;
            _pageState = _pageStates.ForProject(_currentWorkspaceId);
            _RestorePageState();
        }
        else _SyncDraftWorkspace();
    }

    void TaskPage::Refresh()
    {
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->Reload(); });
    }

    void TaskPage::SetProject(const projects::protocol::ProjectProjection& project)
    {
        const auto id = til::u8u16(project.Id);
        if (!_pageStates.Contains(id))
        {
            const auto state = _pageStates.ForProject(id); state->Board = true; state->StatusFilter = 1;
        }
        _projectScoped = true; _projectVersion = project.Version; _projectBound = project.Binding != 0;
        _projectArchived = project.Archived; _projectWorktree = project.Kind != 0;
        CurrentWorkspaceScope().Content(winrt::box_value(RS_(L"Project_CurrentScope")));
        AllWorkspacesScope().Content(winrt::box_value(RS_(L"Project_AllScope")));
        DraftWorkspace().Header(winrt::box_value(RS_(L"Project_Label")));
        const auto root = winrt::hstring{ til::u8u16(project.RootPath) };
        SetCurrentWorkspace(winrt::guid{ project.Id }, root, root);
        _UpdateDraftEnabled();
    }

    void TaskPage::_NewTask_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (_projectScoped && (!_projectBound || _projectArchived)) return;
        _ShowDraft(true);
    }

    void TaskPage::_CancelDraft_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _ShowDraft(false);
    }

    void TaskPage::_SaveDraft_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _CreateDraft(false);
    }

    void TaskPage::_SaveAndRunDraft_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _CreateDraft(true);
    }

    void TaskPage::_TaskList_SelectionChanged(const IInspectable&, const SelectionChangedEventArgs&)
    {
        if (_syncingSelection)
        {
            return;
        }
        _ViewModelImpl()->SelectTask(TaskList().SelectedItem().try_as<TerminalApp::TaskItemViewModel>());
        if (IsLoaded() && TasksSplitView().DisplayMode() == SplitViewDisplayMode::Overlay && TaskList().SelectedItem())
        {
            TasksSplitView().IsPaneOpen(false);
        }
    }

    void TaskPage::_Search_TextChanged(const IInspectable&, const TextChangedEventArgs&)
    {
        if (_restoringPageState) return;
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetSearchText(TaskSearchBox().Text()); });
    }

    void TaskPage::_StatusFilter_SelectionChanged(const IInspectable&, const SelectionChangedEventArgs&)
    {
        if (_restoringPageState) return;
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetStatusFilter(StatusFilter().SelectedIndex()); });
    }

    void TaskPage::_CurrentWorkspaceScope_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (_restoringPageState) return;
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetShowAllWorkspaces(false); });
    }

    void TaskPage::_AllWorkspacesScope_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (_restoringPageState) return;
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetShowAllWorkspaces(true); });
    }

    void TaskPage::_PaneToggle_Click(const IInspectable&, const RoutedEventArgs&)
    {
        TasksSplitView().IsPaneOpen(!TasksSplitView().IsPaneOpen());
    }

    void TaskPage::_Root_KeyDown(const IInspectable&, const KeyRoutedEventArgs& args)
    {
        if (args.Key() == VirtualKey::Escape && AgentManagerHost().Visibility() == Visibility::Visible)
        {
            if (_agentManager) winrt::get_self<implementation::AgentCatalogPage>(_agentManager)->_Back_Click(nullptr, nullptr);
            args.Handled(true); return;
        }
        if (args.Key() == VirtualKey::Escape && DraftLayer().Visibility() == Visibility::Visible)
        {
            if (!_draftSaving) _ShowDraft(false);
            args.Handled(true);
            return;
        }
        if (args.Key() == VirtualKey::Escape && _boardDetailsOpen)
        {
            _CloseBoardDetails_Click(nullptr, nullptr); args.Handled(true); return;
        }
        if (args.Key() == VirtualKey::Escape &&
            TasksSplitView().DisplayMode() == SplitViewDisplayMode::Overlay &&
            TasksSplitView().IsPaneOpen())
        {
            TasksSplitView().IsPaneOpen(false);
            PaneToggle().Focus(FocusState::Programmatic);
            args.Handled(true);
        }
    }

    implementation::TaskPageViewModel* TaskPage::_ViewModelImpl() const noexcept
    {
        return winrt::get_self<implementation::TaskPageViewModel>(_viewModel);
    }

    void TaskPage::_ApplyViewModelUpdate(const std::function<void()>& update)
    {
        const auto wasSyncing = std::exchange(_syncingSelection, true);
        auto restoreSyncing = wil::scope_exit([&]() noexcept { _syncingSelection = wasSyncing; });
        update();
        _SyncSelection();
    }

    void TaskPage::_ShowDraft(const bool visible)
    {
        if (_draftSaving) return;
        if (_draftOutcomeUnknown && _pendingCreation)
        {
            DraftLayer().Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
            _CapturePageState(); return;
        }
        ++_draftGeneration;
        _pendingCreation.reset(); _draftOutcomeUnknown = false;
        DraftError().Text({});
        DraftLayer().Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (visible)
        {
            if (_pageState->Draft.SourceRoot.empty())
            {
                _pageState->Draft.SourceRoot = !_currentRepositoryRoot.empty() ? _currentRepositoryRoot : _currentWorkingDirectory;
                _pageState->Draft.ProjectVersion = _projectVersion;
                if (_projectScoped) DraftExecutionMode().SelectedIndex(_projectWorktree ? 0 : 1);
            }
            _SyncDraftWorkspace();
            winrt::get_self<implementation::AgentSelectionControl>(DraftAgent())->Configure(
                _source ? _source->AgentCatalog() : nullptr, til::u16u8(_currentWorkspaceId));
            DraftTitle().Focus(FocusState::Programmatic);
        }
        else
        {
            _pageState->Draft = {};
            DraftTitle().Text({});
            DraftDescription().Text({});
            DraftAcceptance().Text({});
            NewTaskButton().Focus(FocusState::Programmatic);
        }
        _UpdateDraftEnabled();
        _CapturePageState();
    }

    void TaskPage::_CreateDraft(const bool startImmediately)
    {
        if (_draftSaving || !_source) return;
        if (_draftOutcomeUnknown && _pendingCreation) { _SubmitDraft(_pendingCreation->Draft, _pendingCreation->StartImmediately); return; }
        const auto title = DraftTitle().Text();
        if (title.empty())
        {
            DraftTitle().Focus(FocusState::Programmatic);
            return;
        }

        const auto workspaceIndex = DraftWorkspace().SelectedIndex();
        if (workspaceIndex < 0 || (!_source->IsPrototype() && _currentWorkspaceId.empty()))
        {
            DraftWorkspace().Focus(FocusState::Programmatic);
            return;
        }

        tasks::TaskDraft draft;
        draft.ProjectVersion = _pageState->Draft.ProjectVersion;
        if (_source->IsPrototype())
        {
            draft.Workspace = workspaceIndex == 1 ? L"richerlife-api" : workspaceIndex == 2 ? L"website" :
                                                                                              L"win_terminal";
            draft.WorkspaceId = draft.Workspace;
        }
        else
        {
            draft.WorkspaceId = _currentWorkspaceId;
            const auto root = !_pageState->Draft.SourceRoot.empty() ? _pageState->Draft.SourceRoot :
                              !_currentRepositoryRoot.empty() ? _currentRepositoryRoot : _currentWorkingDirectory;
            draft.Workspace = std::filesystem::path{ root }.filename().wstring();
            if (draft.Workspace.empty())
                draft.Workspace = draft.WorkspaceId;
            draft.SourceRoot = root;
        }
        draft.Title = std::wstring{ title };
        draft.Description = std::wstring{ DraftDescription().Text() };
        draft.Acceptance = _lines(DraftAcceptance().Text());
        draft.UseWorktree = DraftExecutionMode().SelectedIndex() == 0;
        const auto selector = winrt::get_self<implementation::AgentSelectionControl>(DraftAgent());
        const auto selection = selector->Selection();
        if (!selection) { DraftError().Text(RS_(L"Agent_SelectionRequired")); return; }
        draft.Model = til::u8u16(selection->Model); draft.Instructions = til::u8u16(selection->Instructions);
        if (selection->Agent)
        {
            draft.AgentId = til::u8u16(selection->Agent->Id); draft.AgentRevision = selection->Agent->Revision;
            draft.AgentName = selector->SelectedName();
        }
        _SubmitDraft(std::move(draft), startImmediately);
    }

    safe_void_coroutine TaskPage::_Execute(const tasks::TaskCommand command)
    {
        const auto strong = get_strong();
        const auto selectedTask = _viewModel.SelectedTask();
        const auto presenter = _dialogPresenter.get();
        if (command == tasks::TaskCommand::ConfigureAgent) { _ConfigureSelectedAgent(); co_return; }
        if (command == tasks::TaskCommand::EditAcceptance) { _EditAcceptance(); co_return; }
        if (selectedTask && _source && _source->UsesSummaries())
        {
            const auto& task = winrt::get_self<implementation::TaskItemViewModel>(selectedTask)->NativeProjection();
            if (command == tasks::TaskCommand::MarkDone || command == tasks::TaskCommand::Reopen ||
                (command == tasks::TaskCommand::Start && task.Status != tasks::TaskStatus::InProgress))
            {
                _BoardAction({ task, command == tasks::TaskCommand::MarkDone ? tasks::TaskStatus::Done :
                                   command == tasks::TaskCommand::Reopen ? tasks::TaskStatus::Todo : tasks::TaskStatus::InProgress });
                co_return;
            }
        }
        if (command == tasks::TaskCommand::Delete)
        {
            if (!presenter) co_return;
            ContentDialog dialog{};
            dialog.Title(box_value(RS_(L"TaskPage_DeleteTitle")));
            dialog.Content(box_value(RS_(L"TaskPage_DeleteBody")));
            dialog.PrimaryButtonText(RS_(L"TaskPage_DeleteConfirm"));
            dialog.CloseButtonText(RS_(L"TaskPage_DeleteCancel"));
            dialog.DefaultButton(ContentDialogButton::Close);
            const auto weakThis = get_weak();
            if (co_await presenter.ShowDialog(dialog) != ContentDialogResult::Primary ||
                !selectedTask || !_viewModel.SelectedTask() || _viewModel.SelectedTask().Id() != selectedTask.Id())
            {
                co_return;
            }
            if (const auto self = weakThis.get())
            {
                self->_ApplyViewModelUpdate([&]() { self->_ViewModelImpl()->ExecuteSelected(command); });
            }
            co_return;
        }
        if (command == tasks::TaskCommand::CloseTerminal)
        {
            const auto selected = _viewModel.SelectedTask();
            if (selected && selected.HasActiveRun())
            {
                if (!presenter) co_return;
                ContentDialog dialog{};
                dialog.Title(box_value(RS_(L"TaskPage_CloseActiveTitle")));
                dialog.Content(box_value(RS_(L"TaskPage_CloseActiveBody")));
                dialog.PrimaryButtonText(RS_(L"TaskPage_CloseActiveConfirm"));
                dialog.CloseButtonText(RS_(L"TaskPage_CloseActiveCancel"));
                dialog.DefaultButton(ContentDialogButton::Close);
                const auto weakThis = get_weak();
                if (co_await presenter.ShowDialog(dialog) != ContentDialogResult::Primary ||
                    !selectedTask || !_viewModel.SelectedTask() || _viewModel.SelectedTask().Id() != selectedTask.Id())
                {
                    co_return;
                }
                if (const auto self = weakThis.get())
                {
                    self->_ApplyViewModelUpdate([&]() { self->_ViewModelImpl()->ExecuteSelected(command); });
                }
                co_return;
            }
        }

        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->ExecuteSelected(command); });
    }

    void TaskPage::_SyncDraftWorkspace()
    {
        if (_draftSaving || _draftOutcomeUnknown) return;
        if (_source && !_source->IsPrototype())
        {
            const auto root = _pageState && _pageState->Draft.Visible && !_pageState->Draft.SourceRoot.empty() ? _pageState->Draft.SourceRoot :
                              !_currentRepositoryRoot.empty() ? _currentRepositoryRoot : _currentWorkingDirectory;
            auto label = std::filesystem::path{ root }.filename().wstring();
            if (label.empty())
                label = _currentWorkspaceId;
            DraftCurrentWorkspaceItem().Content(box_value(winrt::hstring{ label }));
            DraftPrototypeWorkspaceItemOne().Visibility(Visibility::Collapsed);
            DraftPrototypeWorkspaceItemTwo().Visibility(Visibility::Collapsed);
            DraftWorkspace().SelectedIndex(0);
            return;
        }
        DraftPrototypeWorkspaceItemOne().Visibility(Visibility::Visible);
        DraftPrototypeWorkspaceItemTwo().Visibility(Visibility::Visible);
        int32_t index = -1;
        if (_currentWorkspaceKey == L"win_terminal")
        {
            index = 0;
        }
        else if (_currentWorkspaceKey == L"richerlife-api")
        {
            index = 1;
        }
        else if (_currentWorkspaceKey == L"website")
        {
            index = 2;
        }
        DraftWorkspace().SelectedIndex(index);
    }

    void TaskPage::_SyncSelection()
    {
        const auto wasSyncing = std::exchange(_syncingSelection, true);
        auto restoreSyncing = wil::scope_exit([&]() noexcept { _syncingSelection = wasSyncing; });
        if (TaskList() && TaskList().SelectedItem() != _viewModel.SelectedTask())
        {
            TaskList().SelectedItem(_viewModel.SelectedTask());
        }
    }
}
