// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskPage.h"
#include "TaskDetailsPane.h"

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
        InitializeComponent();
        const auto weakThis = get_weak();
        winrt::get_self<implementation::TaskDetailsPane>(DetailsPane())->SetCommandHandler([weakThis](const auto command) {
            if (const auto self = weakThis.get())
            {
                self->_Execute(command);
            }
        });
        _SyncSelection();
    }

    TerminalApp::TaskPageViewModel TaskPage::ViewModel() const noexcept
    {
        return _viewModel;
    }

    void TaskPage::AttachProjectionSource(std::shared_ptr<tasks::ITaskProjectionSource> source)
    {
        PrototypeSourceInfo().Visibility(source && source->IsPrototype() ? Visibility::Visible : Visibility::Collapsed);
        _source = std::move(source);
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->AttachSource(_source); });
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
        _currentWorkspaceId = id;
        _currentRepositoryRoot = repositoryRoot;
        _currentWorkingDirectory = workingDirectory;
        _currentWorkspaceKey = _source ?
                                   _source->ResolveWorkspaceKey(id, std::wstring_view{ repositoryRoot }, std::wstring_view{ workingDirectory }) :
                                   id;
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetCurrentWorkspace(winrt::hstring{ _currentWorkspaceKey }); });
        _SyncDraftWorkspace();
    }

    void TaskPage::Refresh()
    {
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->Reload(); });
    }

    void TaskPage::_NewTask_Click(const IInspectable&, const RoutedEventArgs&)
    {
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
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetSearchText(TaskSearchBox().Text()); });
    }

    void TaskPage::_StatusFilter_SelectionChanged(const IInspectable&, const SelectionChangedEventArgs&)
    {
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetStatusFilter(StatusFilter().SelectedIndex()); });
    }

    void TaskPage::_CurrentWorkspaceScope_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetShowAllWorkspaces(false); });
    }

    void TaskPage::_AllWorkspacesScope_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _ApplyViewModelUpdate([&]() { _ViewModelImpl()->SetShowAllWorkspaces(true); });
    }

    void TaskPage::_PaneToggle_Click(const IInspectable&, const RoutedEventArgs&)
    {
        TasksSplitView().IsPaneOpen(!TasksSplitView().IsPaneOpen());
    }

    void TaskPage::_Root_KeyDown(const IInspectable&, const KeyRoutedEventArgs& args)
    {
        if (args.Key() == VirtualKey::Escape && DraftLayer().Visibility() == Visibility::Visible)
        {
            _ShowDraft(false);
            args.Handled(true);
            return;
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
        DraftLayer().Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (visible)
        {
            _SyncDraftWorkspace();
            DraftTitle().Focus(FocusState::Programmatic);
        }
        else
        {
            DraftTitle().Text({});
            DraftDescription().Text({});
            DraftAcceptance().Text({});
            NewTaskButton().Focus(FocusState::Programmatic);
        }
    }

    void TaskPage::_CreateDraft(const bool startImmediately)
    {
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
        if (_source->IsPrototype())
        {
            draft.Workspace = workspaceIndex == 1 ? L"richerlife-api" : workspaceIndex == 2 ? L"website" : L"win_terminal";
            draft.WorkspaceId = draft.Workspace;
        }
        else
        {
            draft.WorkspaceId = _currentWorkspaceId;
            const auto root = !_currentRepositoryRoot.empty() ? _currentRepositoryRoot : _currentWorkingDirectory;
            draft.Workspace = std::filesystem::path{ root }.filename().wstring();
            if (draft.Workspace.empty()) draft.Workspace = draft.WorkspaceId;
            draft.SourceRoot = root;
        }
        draft.Title = std::wstring{ title };
        draft.Description = std::wstring{ DraftDescription().Text() };
        draft.Acceptance = _lines(DraftAcceptance().Text());
        draft.UseWorktree = DraftExecutionMode().SelectedIndex() == 0;
        bool created{};
        _ApplyViewModelUpdate([&]() { created = _ViewModelImpl()->CreateTask(draft, startImmediately); });
        if (!created)
        {
            return;
        }
        _ShowDraft(false);
    }

    safe_void_coroutine TaskPage::_Execute(const tasks::TaskCommand command)
    {
        if (command == tasks::TaskCommand::CloseTerminal)
        {
            const auto selected = _viewModel.SelectedTask();
            if (selected && selected.HasActiveRun())
            {
                ContentDialog dialog{};
                dialog.Title(box_value(RS_(L"TaskPage_CloseActiveTitle")));
                dialog.Content(box_value(RS_(L"TaskPage_CloseActiveBody")));
                dialog.PrimaryButtonText(RS_(L"TaskPage_CloseActiveConfirm"));
                dialog.CloseButtonText(RS_(L"TaskPage_CloseActiveCancel"));
                dialog.DefaultButton(ContentDialogButton::Close);
                const auto weakThis = get_weak();
                if (co_await dialog.ShowAsync() != ContentDialogResult::Primary)
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
        if (_source && !_source->IsPrototype())
        {
            const auto root = !_currentRepositoryRoot.empty() ? _currentRepositoryRoot : _currentWorkingDirectory;
            auto label = std::filesystem::path{ root }.filename().wstring();
            if (label.empty()) label = _currentWorkspaceId;
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
