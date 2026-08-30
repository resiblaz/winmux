// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskItemViewModel.h"

#include "TaskItemViewModel.g.cpp"

#include <algorithm>

namespace winrt::TerminalApp::implementation
{
    TaskItemViewModel::TaskItemViewModel() :
        _acceptanceItems{ winrt::single_threaded_observable_vector<winrt::hstring>() },
        _runItems{ winrt::single_threaded_observable_vector<winrt::hstring>() }
    {
    }

    TaskItemViewModel::TaskItemViewModel(const tasks::TaskProjection& projection) :
        TaskItemViewModel{}
    {
        _projection = projection;
        _BuildCollections();
    }

    winrt::hstring TaskItemViewModel::Id() const noexcept
    {
        return winrt::hstring{ _projection.Id };
    }

    winrt::hstring TaskItemViewModel::Workspace() const noexcept
    {
        return winrt::hstring{ _projection.Workspace };
    }

    winrt::hstring TaskItemViewModel::Title() const noexcept
    {
        return winrt::hstring{ _projection.Title };
    }

    winrt::hstring TaskItemViewModel::Description() const noexcept
    {
        return winrt::hstring{ _projection.Description };
    }

    winrt::hstring TaskItemViewModel::StatusLabel() const
    {
        switch (_projection.Status)
        {
        case tasks::TaskStatus::Backlog:
            return RS_(L"TaskPage_StatusBacklog");
        case tasks::TaskStatus::Todo:
            return RS_(L"TaskPage_StatusTodo");
        case tasks::TaskStatus::InProgress:
            return RS_(L"TaskPage_StatusInProgress");
        case tasks::TaskStatus::InReview:
            return RS_(L"TaskPage_StatusInReview");
        case tasks::TaskStatus::Blocked:
            return RS_(L"TaskPage_StatusBlocked");
        case tasks::TaskStatus::Done:
            return RS_(L"TaskPage_StatusDone");
        case tasks::TaskStatus::Cancelled:
            return RS_(L"TaskPage_StatusCancelled");
        }
        return {};
    }

    winrt::hstring TaskItemViewModel::StatusGlyph() const
    {
        switch (_projection.Status)
        {
        case tasks::TaskStatus::InProgress:
            return L"↻";
        case tasks::TaskStatus::InReview:
            return L"◇";
        case tasks::TaskStatus::Blocked:
            return L"!";
        case tasks::TaskStatus::Done:
            return L"✓";
        case tasks::TaskStatus::Cancelled:
            return L"×";
        default:
            return L"·";
        }
    }

    winrt::hstring TaskItemViewModel::PriorityLabel() const
    {
        switch (_projection.Priority)
        {
        case tasks::TaskPriority::Low:
            return RS_(L"TaskPage_PriorityLow");
        case tasks::TaskPriority::High:
            return RS_(L"TaskPage_PriorityHigh");
        default:
            return RS_(L"TaskPage_PriorityMedium");
        }
    }

    winrt::hstring TaskItemViewModel::UpdatedLabel() const noexcept
    {
        return winrt::hstring{ _projection.UpdatedLabel };
    }

    winrt::hstring TaskItemViewModel::AcceptanceSummary() const
    {
        const auto completed = std::count_if(_projection.Acceptance.begin(), _projection.Acceptance.end(), [](const auto& item) {
            return item.Done;
        });
        return winrt::hstring{ RS_fmt(L"TaskPage_AcceptanceSummaryFormat", completed, _projection.Acceptance.size()) };
    }

    winrt::hstring TaskItemViewModel::ExecutionMode() const noexcept
    {
        return winrt::hstring{ _projection.ExecutionMode };
    }

    winrt::hstring TaskItemViewModel::TerminalLabel() const
    {
        switch (_projection.TerminalState)
        {
        case tasks::TaskTerminalState::NotCreated:
            return RS_(L"TaskPage_TerminalNotCreated");
        case tasks::TaskTerminalState::Starting:
            return RS_(L"TaskPage_TerminalStarting");
        case tasks::TaskTerminalState::Ready:
            return RS_(L"TaskPage_TerminalReady");
        case tasks::TaskTerminalState::Busy:
            return RS_(L"TaskPage_TerminalBusy");
        case tasks::TaskTerminalState::Suspended:
            return RS_(L"TaskPage_TerminalSuspended");
        case tasks::TaskTerminalState::Closed:
            return RS_(L"TaskPage_TerminalClosed");
        case tasks::TaskTerminalState::Error:
            return RS_(L"TaskPage_TerminalError");
        }
        return {};
    }

    winrt::hstring TaskItemViewModel::WorkingDirectory() const noexcept
    {
        return winrt::hstring{ _projection.WorkingDirectory };
    }

    winrt::hstring TaskItemViewModel::Worktree() const noexcept
    {
        return winrt::hstring{ _projection.Worktree };
    }

    winrt::hstring TaskItemViewModel::Branch() const noexcept
    {
        return winrt::hstring{ _projection.Branch };
    }

    winrt::hstring TaskItemViewModel::Result() const noexcept
    {
        return winrt::hstring{ _projection.Result };
    }

    winrt::hstring TaskItemViewModel::ActiveRunSummary() const noexcept
    {
        return winrt::hstring{ _projection.ActiveRunSummary };
    }

    winrt::hstring TaskItemViewModel::AutomationName() const
    {
        return winrt::hstring{ RS_fmt(L"TaskPage_TaskAutomationNameFormat", Id(), Title(), StatusLabel(), Workspace()) };
    }

    bool TaskItemViewModel::HasActiveRun() const noexcept
    {
        return _projection.HasActiveRun;
    }

    bool TaskItemViewModel::HasResult() const noexcept
    {
        return !_projection.Result.empty();
    }

    bool TaskItemViewModel::CanStart() const noexcept
    {
        return !_projection.HasActiveRun &&
               (_projection.Status == tasks::TaskStatus::Backlog ||
                _projection.Status == tasks::TaskStatus::Todo ||
                _projection.Status == tasks::TaskStatus::InReview);
    }

    bool TaskItemViewModel::CanStop() const noexcept
    {
        return _projection.HasActiveRun;
    }

    bool TaskItemViewModel::CanOpenTerminal() const noexcept
    {
        return _projection.TerminalState != tasks::TaskTerminalState::NotCreated;
    }

    bool TaskItemViewModel::CanCloseTerminal() const noexcept
    {
        return _projection.TerminalState != tasks::TaskTerminalState::NotCreated &&
               _projection.TerminalState != tasks::TaskTerminalState::Closed;
    }

    bool TaskItemViewModel::CanMarkDone() const noexcept
    {
        return _projection.Status == tasks::TaskStatus::InReview && !_projection.HasActiveRun;
    }

    bool TaskItemViewModel::CanReopen() const noexcept
    {
        return _projection.Status == tasks::TaskStatus::Done || _projection.Status == tasks::TaskStatus::Cancelled;
    }

    Windows::Foundation::Collections::IObservableVector<winrt::hstring> TaskItemViewModel::AcceptanceItems() const noexcept
    {
        return _acceptanceItems;
    }

    Windows::Foundation::Collections::IObservableVector<winrt::hstring> TaskItemViewModel::RunItems() const noexcept
    {
        return _runItems;
    }

    tasks::TaskStatus TaskItemViewModel::NativeStatus() const noexcept
    {
        return _projection.Status;
    }

    void TaskItemViewModel::_BuildCollections()
    {
        _acceptanceItems.Clear();
        for (const auto& item : _projection.Acceptance)
        {
            _acceptanceItems.Append(winrt::hstring{ fmt::format(FMT_COMPILE(L"{} {}"), item.Done ? L"✓" : L"○", item.Text) });
        }

        _runItems.Clear();
        for (const auto& run : _projection.Runs)
        {
            _runItems.Append(winrt::hstring{ run });
        }
    }
}
