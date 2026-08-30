// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskPage.g.h"
#include "ITaskProjectionSource.h"
#include "TaskPageViewModel.h"

#include <functional>

namespace winrt::TerminalApp::implementation
{
    struct TaskPage : TaskPageT<TaskPage>
    {
        TaskPage();

        TerminalApp::TaskPageViewModel ViewModel() const noexcept;
        void AttachProjectionSource(std::shared_ptr<tasks::ITaskProjectionSource> source);
        void SetCurrentWorkspace(const winrt::guid& workspaceId,
                                 const winrt::hstring& repositoryRoot,
                                 const winrt::hstring& workingDirectory);
        void Refresh();

        void _NewTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
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

    private:
        TerminalApp::TaskPageViewModel _viewModel;
        std::shared_ptr<tasks::ITaskProjectionSource> _source;
        std::wstring _currentWorkspaceKey{ L"win_terminal" };
        bool _syncingSelection{};

        implementation::TaskPageViewModel* _ViewModelImpl() const noexcept;
        void _ApplyViewModelUpdate(const std::function<void()>& update);
        void _ShowDraft(bool visible);
        void _CreateDraft(bool startImmediately);
        safe_void_coroutine _Execute(tasks::TaskCommand command);
        void _SyncDraftWorkspace();
        void _SyncSelection();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TaskPage);
}
