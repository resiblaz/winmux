// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceSidebar.g.h"

namespace winrt::TerminalApp::implementation
{
    struct WorkspaceSidebar : WorkspaceSidebarT<WorkspaceSidebar>
    {
        WorkspaceSidebar();

        bool IsExpanded() const noexcept;
        void IsExpanded(bool value);
        double ExpandedWidth() const noexcept;
        void ExpandedWidth(double value);
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> NotificationHistory() const noexcept;
        void RefreshNotifications();

        til::property_changed_event PropertyChanged;
        til::typed_event<TerminalApp::WorkspaceSidebar, Windows::Foundation::IInspectable> NewRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> PinRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> CloseRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> CloseOthersRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> RenameRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ActivateNotificationRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceNotification> ActivateNotificationItemRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> MarkNotificationsReadRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ClearNotificationHistoryRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, Windows::Foundation::IInspectable> MarkAllNotificationsReadRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, Windows::Foundation::IInspectable> ClearAllNotificationHistoryRequested;

        void _Toggle_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Resize_DragDelta(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::Primitives::DragDeltaEventArgs& args);
        void _New_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Pin_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Close_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CloseOthers_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Rename_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Notification_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _MarkNotificationsRead_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ClearNotificationHistory_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _NotificationCenter_Opening(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& args);
        void _NotificationHistory_ItemClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::ItemClickEventArgs& args);
        void _MarkAllNotificationsRead_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ClearAllNotificationHistory_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

    private:
        static constexpr double MinimumExpandedWidth{ 220.0 };
        static constexpr double MaximumExpandedWidth{ 480.0 };
        static constexpr double CollapsedWidth{ 48.0 };

        bool _isExpanded{ true };
        double _expandedWidth{ 280.0 };
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> _notificationHistory;

        void _ApplyLayout();
        TerminalApp::WorkspaceItemViewModel _ItemFromSender(const Windows::Foundation::IInspectable& sender) const;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(WorkspaceSidebar);
}
