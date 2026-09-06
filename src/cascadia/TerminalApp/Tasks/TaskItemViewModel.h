// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskItemViewModel.g.h"
#include "TaskProjection.h"

namespace winrt::TerminalApp::implementation
{
    struct TaskItemViewModel : TaskItemViewModelT<TaskItemViewModel>
    {
        TaskItemViewModel();
        explicit TaskItemViewModel(const tasks::TaskProjection& projection);

        winrt::hstring Id() const noexcept;
        winrt::hstring Workspace() const noexcept;
        winrt::hstring Title() const noexcept;
        winrt::hstring Description() const noexcept;
        winrt::hstring StatusLabel() const;
        winrt::hstring StatusGlyph() const;
        winrt::hstring PriorityLabel() const;
        winrt::hstring UpdatedLabel() const noexcept;
        winrt::hstring AcceptanceSummary() const;
        winrt::hstring ExecutionMode() const noexcept;
        winrt::hstring AgentSummary() const;
        winrt::hstring TerminalLabel() const;
        winrt::hstring WorkingDirectory() const noexcept;
        winrt::hstring Worktree() const noexcept;
        winrt::hstring Branch() const noexcept;
        winrt::hstring Result() const noexcept;
        winrt::hstring ActiveRunSummary() const noexcept;
        winrt::hstring AutomationName() const;
        bool HasActiveRun() const noexcept;
        bool HasResult() const noexcept;
        bool HasQueuedMessages() const noexcept;
        bool HasRunDetails() const noexcept;
        bool CanStart() const noexcept;
        bool CanStop() const noexcept;
        bool CanOpenTerminal() const noexcept;
        bool CanCloseTerminal() const noexcept;
        bool CanMarkDone() const noexcept;
        bool CanReopen() const noexcept;
        bool CanRetry() const noexcept;
        bool CanCancel() const noexcept;
        bool CanFinalizeWorktree() const noexcept;
        bool CanDelete() const noexcept;
        bool CanConfigureAgent() const noexcept;
        Windows::Foundation::Collections::IObservableVector<winrt::hstring> AcceptanceItems() const noexcept;
        Windows::Foundation::Collections::IObservableVector<winrt::hstring> RunItems() const noexcept;
        Windows::Foundation::Collections::IObservableVector<winrt::hstring> QueuedMessageItems() const noexcept;
        Windows::Foundation::Collections::IObservableVector<winrt::hstring> RunDetailItems() const noexcept;
        uint64_t Version() const noexcept;

        tasks::TaskStatus NativeStatus() const noexcept;
        std::optional<std::pair<std::wstring, uint64_t>> QueuedMessageIdentityAt(uint32_t index) const;

    private:
        tasks::TaskProjection _projection;
        Windows::Foundation::Collections::IObservableVector<winrt::hstring> _acceptanceItems;
        Windows::Foundation::Collections::IObservableVector<winrt::hstring> _runItems;
        Windows::Foundation::Collections::IObservableVector<winrt::hstring> _queuedMessageItems;
        Windows::Foundation::Collections::IObservableVector<winrt::hstring> _runDetailItems;

        void _BuildCollections();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TaskItemViewModel);
}
