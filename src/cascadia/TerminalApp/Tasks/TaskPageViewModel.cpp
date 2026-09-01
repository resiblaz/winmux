// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskPageViewModel.h"

#include "TaskPageViewModel.g.cpp"

#include <algorithm>
#include <cwctype>

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        std::wstring _foldCase(const std::wstring_view value)
        {
            std::wstring folded{ value };
            std::transform(folded.begin(), folded.end(), folded.begin(), [](const wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
            return folded;
        }

        bool _contains(const std::wstring_view value, const std::wstring_view query)
        {
            return query.empty() || _foldCase(value).find(_foldCase(query)) != std::wstring::npos;
        }
    }

    TaskPageViewModel::TaskPageViewModel() :
        _tasks{ winrt::single_threaded_observable_vector<TerminalApp::TaskItemViewModel>() }
    {
    }

    TaskPageViewModel::~TaskPageViewModel()
    {
        if (_source)
        {
            _source->SetChangedHandler({});
            _source->Stop();
            _source.reset();
        }
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::TaskItemViewModel> TaskPageViewModel::Tasks() const noexcept
    {
        return _tasks;
    }

    TerminalApp::TaskItemViewModel TaskPageViewModel::SelectedTask() const noexcept
    {
        return _selectedTask;
    }

    winrt::hstring TaskPageViewModel::TaskCountText() const
    {
        return winrt::hstring{ RS_fmt(L"TaskPage_TaskCountFormat", _tasks.Size()) };
    }

    bool TaskPageViewModel::IsEmpty() const noexcept
    {
        return _tasks.Size() == 0;
    }

    bool TaskPageViewModel::IsLoading() const noexcept
    {
        return _source && _source->IsLoading();
    }

    bool TaskPageViewModel::ShowEmptyState() const noexcept
    {
        return IsEmpty() && !IsLoading();
    }

    bool TaskPageViewModel::HasSelection() const noexcept
    {
        return _selectedTask != nullptr;
    }

    bool TaskPageViewModel::IsSourceAttached() const noexcept
    {
        return _source != nullptr;
    }

    void TaskPageViewModel::AttachSource(std::shared_ptr<tasks::ITaskProjectionSource> source)
    {
        if (_source == source)
        {
            return;
        }

        _source = std::move(source);
        if (_source)
        {
            const auto weakThis = get_weak();
            _source->SetChangedHandler([weakThis] {
                if (const auto self = weakThis.get())
                    self->Reload();
            });
            if (!_source->Start())
            {
                _source->SetChangedHandler({});
                _source.reset();
            }
        }
        Reload();
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsSourceAttached" });
    }

    void TaskPageViewModel::DetachSource()
    {
        if (!_source)
        {
            return;
        }

        auto source = std::move(_source);
        source->SetChangedHandler({});
        source->Stop();
        _snapshot.clear();
        _ApplyFilters();
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsSourceAttached" });
    }

    void TaskPageViewModel::Reload()
    {
        _snapshot = _source ? _source->Snapshot() : std::vector<tasks::TaskProjection>{};
        _ApplyFilters();
    }

    void TaskPageViewModel::SelectTask(const TerminalApp::TaskItemViewModel& task)
    {
        const auto nextId = task ? std::wstring{ task.Id() } : std::wstring{};
        if (_selectedTask == task && _selectedTaskId == nextId)
        {
            return;
        }

        _selectedTask = task;
        _selectedTaskId = std::move(nextId);
        if (_source)
            _source->SelectTask(_selectedTaskId);
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SelectedTask" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"HasSelection" });
    }

    void TaskPageViewModel::SetSearchText(const winrt::hstring& value)
    {
        const std::wstring next{ value };
        if (_searchText != next)
        {
            _searchText = next;
            _ApplyFilters();
        }
    }

    void TaskPageViewModel::SetShowAllWorkspaces(const bool value)
    {
        if (_showAllWorkspaces != value)
        {
            _showAllWorkspaces = value;
            _ApplyFilters();
        }
    }

    void TaskPageViewModel::SetStatusFilter(const int32_t value)
    {
        if (_statusFilter != value)
        {
            _statusFilter = value;
            _ApplyFilters();
        }
    }

    void TaskPageViewModel::SetCurrentWorkspace(const winrt::hstring& workspace)
    {
        const std::wstring next{ workspace };
        if (_currentWorkspace != next)
        {
            _currentWorkspace = next;
            if (!_showAllWorkspaces)
            {
                _ApplyFilters();
            }
        }
    }

    bool TaskPageViewModel::CreateTask(const tasks::TaskDraft& draft, const bool startImmediately)
    {
        if (!_source || draft.Title.empty())
        {
            return false;
        }

        const auto created = _source->CreateTask(draft, startImmediately);
        _selectedTaskId = created.Id;
        Reload();
        return true;
    }

    void TaskPageViewModel::ExecuteSelected(const tasks::TaskCommand command)
    {
        if (!_source || _selectedTaskId.empty())
        {
            return;
        }

        if (_source->Execute(_selectedTaskId, command))
        {
            Reload();
        }
    }

    bool TaskPageViewModel::QueueSelectedMessage(const winrt::hstring& message)
    {
        if (!_source || !_selectedTask || message.empty())
        {
            return false;
        }
        const auto queued = _source->QueueMessage(
            std::wstring{ _selectedTask.Id() },
            std::wstring{ message });
        if (queued)
        {
            Reload();
        }
        return queued;
    }

    bool TaskPageViewModel::ReviseSelectedMessage(
        const std::wstring& messageId,
        const uint64_t revision,
        const std::optional<std::wstring>& replacement)
    {
        if (!_source || !_selectedTask || messageId.empty() || revision == 0)
            return false;
        const auto revised = _source->ReviseMessage(
            std::wstring{ _selectedTask.Id() }, messageId, revision, replacement);
        if (revised)
            Reload();
        return revised;
    }

    void TaskPageViewModel::_ApplyFilters()
    {
        _tasks.Clear();
        for (const auto& task : _snapshot)
        {
            if (_Matches(task))
            {
                _tasks.Append(winrt::make<implementation::TaskItemViewModel>(task));
            }
        }

        TerminalApp::TaskItemViewModel nextSelection{ nullptr };
        if (!_selectedTaskId.empty())
        {
            for (const auto& task : _tasks)
            {
                if (std::wstring_view{ task.Id() } == _selectedTaskId)
                {
                    nextSelection = task;
                    break;
                }
            }
        }
        if (!nextSelection && _tasks.Size() > 0)
        {
            nextSelection = _tasks.GetAt(0);
        }

        _selectedTask = nextSelection;
        if (nextSelection)
        {
            _selectedTaskId = std::wstring{ nextSelection.Id() };
        }
        else
        {
            const auto selectedStillExists = std::any_of(_snapshot.begin(), _snapshot.end(), [&](const auto& task) {
                return task.Id == _selectedTaskId;
            });
            if (!selectedStillExists)
            {
                _selectedTaskId.clear();
            }
        }
        if (_source)
            _source->SelectTask(_selectedTaskId);
        _RaisePageState();
    }

    void TaskPageViewModel::_RaisePageState()
    {
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"Tasks" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SelectedTask" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"TaskCountText" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsEmpty" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsLoading" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"ShowEmptyState" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"HasSelection" });
    }

    bool TaskPageViewModel::_Matches(const tasks::TaskProjection& task) const
    {
        const auto workspaceKey = task.WorkspaceKey.empty() ? std::wstring_view{ task.Workspace } : std::wstring_view{ task.WorkspaceKey };
        if (!_showAllWorkspaces && workspaceKey != _currentWorkspace)
        {
            return false;
        }
        if (!_MatchesStatus(task.Status))
        {
            return false;
        }
        return _searchText.empty() || _contains(task.Id, _searchText) || _contains(task.Title, _searchText) || _contains(task.Workspace, _searchText);
    }

    bool TaskPageViewModel::_MatchesStatus(const tasks::TaskStatus status) const noexcept
    {
        switch (_statusFilter)
        {
        case 0:
            return status != tasks::TaskStatus::Done && status != tasks::TaskStatus::Cancelled;
        case 1:
            return true;
        case 2:
            return status == tasks::TaskStatus::Todo || status == tasks::TaskStatus::Backlog;
        case 3:
            return status == tasks::TaskStatus::InProgress;
        case 4:
            return status == tasks::TaskStatus::InReview;
        case 5:
            return status == tasks::TaskStatus::Blocked;
        case 6:
            return status == tasks::TaskStatus::Done;
        default:
            return true;
        }
    }
}
