// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceItemViewModel.g.h"
#include "WorkspaceNotification.h"

namespace winrt::TerminalApp::implementation
{
    struct WorkspaceItemViewModel : WorkspaceItemViewModelT<WorkspaceItemViewModel>
    {
        WorkspaceItemViewModel(const TerminalApp::Tab& tab);

        winrt::guid WorkspaceId() const noexcept;
        TerminalApp::Tab SourceTab() const noexcept;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> Surfaces() const noexcept;
        uint32_t SurfaceCount() const noexcept;
        winrt::hstring SurfaceCountText() const;
        bool HasMultipleSurfaces() const noexcept;
        winrt::hstring Title() const;
        winrt::hstring CustomName() const;
        void CustomName(const winrt::hstring& value);
        winrt::hstring AutomationName() const;
        winrt::hstring Icon() const;
        bool IsPinned() const;
        void IsPinned(bool value);
        winrt::hstring PinIconGlyph() const;
        winrt::hstring PinActionText() const;
        winrt::hstring PinAutomationName() const;
        winrt::hstring SwitchShortcutText() const;
        winrt::hstring SwitchShortcutHintText() const;
        winrt::hstring UnreadBadgeText() const;
        winrt::hstring NotificationAutomationName() const;
        uint32_t NotificationHistoryCount() const noexcept;
        winrt::hstring NotificationHistoryCountText() const;
        winrt::hstring WorkingDirectoryDisplay() const;
        winrt::hstring ListeningPortsDisplay() const;
        winrt::hstring ListeningPortsTooltip() const;
        bool HasAgentResume() const noexcept;
        winrt::hstring AgentResumeProvider() const noexcept;
        winrt::hstring AgentResumeSessionId() const noexcept;
        winrt::hstring AgentResumeWorkingDirectory() const noexcept;
        TerminalApp::WorkspaceNotification LatestUnreadNotification() const;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> Notifications() const noexcept;
        void RestoreWorkspaceId(const winrt::guid& workspaceId) noexcept;
        winrt::guid AddSurface(const TerminalApp::Tab& tab, std::optional<winrt::guid> surfaceId = std::nullopt);
        bool RemoveSurface(const TerminalApp::Tab& tab);
        bool MoveSurface(const TerminalApp::Tab& tab, uint32_t targetIndex);
        bool ContainsTab(const TerminalApp::Tab& tab) const noexcept;
        void SetActiveTab(const TerminalApp::Tab& tab);
        winrt::guid SurfaceIdForTab(const TerminalApp::Tab& tab) const noexcept;
        TerminalApp::Tab TabForSurfaceId(const winrt::guid& surfaceId) const noexcept;
        void RestoreSurfaceId(const TerminalApp::Tab& tab, const winrt::guid& surfaceId);
        void BindSourceSnapshot(const winrt::guid& snapshotId, uint64_t revision) noexcept;
        void ClearSourceSnapshot() noexcept;
        std::optional<winrt::guid> SourceSnapshotId() const noexcept;
        uint64_t SourceSnapshotRevision() const noexcept;
        void ShowSwitchShortcutHint(bool value);
        void UpdateSwitchShortcut(const winrt::hstring& shortcut);
        void RefreshGitBranch();
        void UpdateListeningPorts(std::vector<uint16_t> ports);
        const std::vector<uint16_t>& ListeningPorts() const noexcept;

        void AddNotification(const winrt::guid& surfaceId, uint32_t paneId, const winrt::hstring& title, const winrt::hstring& message);
        void MarkAllNotificationsRead();
        void MarkNotificationsReadForPane(uint32_t paneId);
        void MarkNotificationsReadForSurfacePane(const winrt::guid& surfaceId, uint32_t paneId);
        void ClearNotificationHistory();
        void ConfigureAgentResume(const winrt::hstring& provider, const winrt::hstring& sessionId, const winrt::hstring& workingDirectory);
        void ClearAgentResume();

        void SetAutomationStatus(const winrt::hstring& text);
        void ClearAutomationStatus();
        winrt::hstring AutomationStatus() const noexcept;
        void SetAutomationProgress(uint64_t state, uint32_t value);
        void ClearAutomationProgress();
        uint64_t AutomationProgressState() const noexcept;
        uint32_t AutomationProgressValue() const noexcept;

        til::property_changed_event PropertyChanged;
        bool IsSelected() const noexcept;
        void IsSelected(bool value);
        WINRT_OBSERVABLE_PROPERTY(uint32_t, UnreadCount, PropertyChanged.raise, 0);
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, WorkingDirectory, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, GitBranch, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, GitRepositoryRoot, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(bool, IsGitBranchLoading, PropertyChanged.raise, false);
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, StatusText, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, LatestNotification, PropertyChanged.raise, L"");

    private:
        struct SurfaceEntry
        {
            winrt::guid Id{};
            TerminalApp::Tab Tab{ nullptr };
        };

        winrt::guid _workspaceId{};
        winrt::hstring _customName;
        winrt::hstring _switchShortcutText;
        TerminalApp::Tab _tab{ nullptr };
        TerminalApp::Tab _directoryTab{ nullptr };
        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _surfaces;
        std::vector<SurfaceEntry> _surfaceEntries;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> _notifications;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _tabChangedRevoker;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _directoryTabChangedRevoker;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _tabStatusChangedRevoker;
        Windows::System::DispatcherQueue _dispatcher{ nullptr };
        bool _isSelected{ false };
        bool _showSwitchShortcutHint{ false };
        uint64_t _gitBranchRequestGeneration{ 0 };
        std::optional<winrt::guid> _sourceSnapshotId;
        uint64_t _sourceSnapshotRevision{};
        winrt::hstring _agentResumeProvider;
        winrt::hstring _agentResumeSessionId;
        winrt::hstring _agentResumeWorkingDirectory;
        winrt::hstring _automationStatus;
        uint64_t _automationProgressState{ 0 };
        uint32_t _automationProgressValue{ 0 };
        std::vector<uint16_t> _listeningPorts;

        static constexpr uint32_t MaximumNotifications{ 100 };
        static constexpr size_t MaximumDisplayedListeningPorts{ 3 };
        static constexpr auto GitQueryTimeout{ std::chrono::milliseconds{ 1500 } };
        static constexpr auto GitCacheMaximumAge{ std::chrono::seconds{ 10 } };
        safe_void_coroutine _RefreshGitBranchAsync(winrt::hstring directory, uint64_t generation);
        void _AttachActiveTab(const TerminalApp::Tab& tab);
        void _AttachDirectoryTab();
        void _RaiseActiveTabProperties();
        void _RaiseSurfaceProperties();
        void _UpdateWorkingDirectory(bool refreshGit = true);
        void _UpdateStatus();
        void _UpdateNotificationProperties();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(WorkspaceItemViewModel);
}
