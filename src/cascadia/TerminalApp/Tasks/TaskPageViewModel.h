// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskPageViewModel.g.h"
#include "ITaskProjectionSource.h"
#include "TaskItemViewModel.h"

namespace winrt::TerminalApp::implementation
{
    struct TaskPageViewModel : TaskPageViewModelT<TaskPageViewModel>
    {
        TaskPageViewModel();

        Windows::Foundation::Collections::IObservableVector<TerminalApp::TaskItemViewModel> Tasks() const noexcept;
        TerminalApp::TaskItemViewModel SelectedTask() const noexcept;
        winrt::hstring TaskCountText() const;
        bool IsEmpty() const noexcept;
        bool HasSelection() const noexcept;
        bool IsSourceAttached() const noexcept;

        void AttachSource(std::shared_ptr<tasks::ITaskProjectionSource> source);
        void DetachSource();
        void Reload();
        void SelectTask(const TerminalApp::TaskItemViewModel& task);
        void SetSearchText(const winrt::hstring& value);
        void SetShowAllWorkspaces(bool value);
        void SetStatusFilter(int32_t value);
        void SetCurrentWorkspace(const winrt::hstring& workspace);
        bool CreateTask(const tasks::TaskDraft& draft, bool startImmediately);
        void ExecuteSelected(tasks::TaskCommand command);

        til::property_changed_event PropertyChanged;

    private:
        std::shared_ptr<tasks::ITaskProjectionSource> _source;
        std::vector<tasks::TaskProjection> _snapshot;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::TaskItemViewModel> _tasks;
        TerminalApp::TaskItemViewModel _selectedTask{ nullptr };
        std::wstring _selectedTaskId;
        std::wstring _searchText;
        std::wstring _currentWorkspace{ L"win_terminal" };
        bool _showAllWorkspaces{};
        int32_t _statusFilter{};

        void _ApplyFilters();
        void _RaisePageState();
        [[nodiscard]] bool _Matches(const tasks::TaskProjection& task) const;
        [[nodiscard]] bool _MatchesStatus(tasks::TaskStatus status) const noexcept;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TaskPageViewModel);
}
