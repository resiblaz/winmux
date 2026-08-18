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
        winrt::hstring Title() const;
        winrt::hstring Icon() const;
        bool IsPinned() const;
        void IsPinned(bool value);
        winrt::hstring PinIconGlyph() const;
        winrt::hstring PinActionText() const;
        winrt::hstring PinAutomationName() const;
        winrt::hstring SwitchShortcutText() const;
        winrt::hstring UnreadBadgeText() const;
        winrt::hstring NotificationAutomationName() const;
        uint32_t NotificationHistoryCount() const noexcept;
        winrt::hstring NotificationHistoryCountText() const;
        winrt::hstring WorkingDirectoryDisplay() const;
        TerminalApp::WorkspaceNotification LatestUnreadNotification() const;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> Notifications() const noexcept;
        void RestoreWorkspaceId(const winrt::guid& workspaceId) noexcept;

        void AddNotification(uint32_t paneId, const winrt::hstring& title, const winrt::hstring& message);
        void MarkAllNotificationsRead();
        void MarkNotificationsReadForPane(uint32_t paneId);
        void ClearNotificationHistory();

        til::property_changed_event PropertyChanged;
        WINRT_OBSERVABLE_PROPERTY(bool, IsSelected, PropertyChanged.raise, false);
        WINRT_OBSERVABLE_PROPERTY(uint32_t, UnreadCount, PropertyChanged.raise, 0);
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, WorkingDirectory, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, StatusText, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, LatestNotification, PropertyChanged.raise, L"");

    private:
        winrt::guid _workspaceId{};
        TerminalApp::Tab _tab{ nullptr };
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> _notifications;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _tabChangedRevoker;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _tabStatusChangedRevoker;

        static constexpr uint32_t MaximumNotifications{ 100 };
        void _UpdateWorkingDirectory();
        void _UpdateStatus();
        void _UpdateNotificationProperties();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(WorkspaceItemViewModel);
}
