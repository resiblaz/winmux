// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskPage.g.h"
#include "ITaskProjectionSource.h"
#include "TaskPageViewModel.h"
#include "TaskPageState.h"
#include "TaskBoardView.h"

#include <functional>

namespace winrt::TerminalApp::implementation
{
    struct TaskPage : TaskPageT<TaskPage>
    {
        TaskPage();

        TerminalApp::TaskPageViewModel ViewModel() const noexcept;
        void AttachProjectionSource(std::shared_ptr<tasks::ITaskProjectionSource> source);
        void SetSurfaceHandler(tasks::ITaskProjectionSource::SurfaceHandler handler);
        void SetDialogPresenter(const winrt::TerminalApp::IDialogPresenter& presenter)
        {
            _dialogPresenter = presenter;
        }
        void SetCurrentWorkspace(const winrt::guid& workspaceId,
                                 const winrt::hstring& repositoryRoot,
                                 const winrt::hstring& workingDirectory);
        void SetProject(const projects::protocol::ProjectProjection& project);
        void Refresh();

        void _NewTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ManageAgents_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CancelDraft_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _SaveDraft_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _SaveAndRunDraft_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _TaskList_SelectionChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);
        void _Search_TextChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::TextChangedEventArgs& args);
        void _StatusFilter_SelectionChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);
        void _CurrentWorkspaceScope_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _AllWorkspacesScope_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _PaneToggle_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Root_KeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& args);
        void _BoardMode_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _ListMode_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _CloseBoardDetails_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _LoadMoreTasks_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _RetryBoardAction_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _UndoBoardAction_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _BoardAuxiliary_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);

    private:
        TerminalApp::TaskPageViewModel _viewModel;
        std::shared_ptr<tasks::ITaskProjectionSource> _source;
        std::wstring _currentWorkspaceKey{ L"win_terminal" };
        std::wstring _currentWorkspaceId;
        std::wstring _currentRepositoryRoot;
        std::wstring _currentWorkingDirectory;
        uint64_t _projectVersion{};
        bool _projectScoped{};
        bool _projectBound{};
        bool _projectArchived{};
        bool _projectWorktree{};
        bool _syncingSelection{};
        bool _restoringPageState{ true };
        tasks::TaskPageStateStore _pageStates;
        std::shared_ptr<tasks::TaskPageState> _pageState;
        tasks::ITaskProjectionSource::SurfaceHandler _surfaceHandler;
        winrt::weak_ref<winrt::TerminalApp::IDialogPresenter> _dialogPresenter;
        TerminalApp::AgentCatalogPage _agentManager{ nullptr };
        std::optional<tasks::TaskCreationAttempt> _pendingCreation;
        bool _draftSaving{};
        bool _draftOutcomeUnknown{};
        uint64_t _draftGeneration{};
        std::shared_ptr<tasks::TaskActionController> _actions;
        Windows::UI::Xaml::DispatcherTimer _boardPoll{ nullptr };
        bool _boardDetailsOpen{};
        void _InitializeBoard();
        void _SetBoardMode(bool board);
        void _UpdateBoard();
        safe_void_coroutine _BoardAction(TaskBoardDrop drop);
        safe_void_coroutine _EditAcceptance();

        implementation::TaskPageViewModel* _ViewModelImpl() const noexcept;
        void _ApplyViewModelUpdate(const std::function<void()>& update);
        void _ShowDraft(bool visible);
        void _CreateDraft(bool startImmediately);
        void _SubmitDraft(tasks::TaskDraft draft, bool startImmediately);
        void _UpdateDraftEnabled();
        safe_void_coroutine _ConfigureSelectedAgent();
        safe_void_coroutine _Execute(tasks::TaskCommand command);
        void _SyncDraftWorkspace();
        void _SyncSelection();
        void _CapturePageState();
        void _RestorePageState();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TaskPage);
}
