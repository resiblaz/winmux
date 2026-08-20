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
        void SetWindowNarrow(bool value);
        bool IsWindowNarrow() const noexcept;
        bool IsEffectivelyExpanded() const noexcept;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> NotificationHistory() const noexcept;
        void RefreshNotifications();
        void OpenNotificationCenter(const Windows::UI::Xaml::FrameworkElement& target);
        void ShowSwitchHints(bool value);

        static constexpr double NarrowWindowThreshold{ 640.0 };

        til::property_changed_event PropertyChanged;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> PinRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> CloseRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> CloseOthersRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> RenameRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ConfigureAgentResumeRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ResumeAgentRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ClearAgentResumeRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ActivateNotificationRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceNotification> ActivateNotificationItemRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> MarkNotificationsReadRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ClearNotificationHistoryRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, Windows::Foundation::IInspectable> MarkAllNotificationsReadRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, Windows::Foundation::IInspectable> ClearAllNotificationHistoryRequested;

        void _Toggle_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Resize_DragDelta(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::Primitives::DragDeltaEventArgs& args);
        void _Resize_PreviewKeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& args);
        void _WorkspaceList_KeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& args);
        void _WorkspaceItem_PointerEntered(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        void _WorkspaceItem_PointerExited(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        void _Pin_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Close_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CloseOthers_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Rename_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ConfigureAgentResume_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ResumeAgent_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ClearAgentResume_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Notification_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _MarkNotificationsRead_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ClearNotificationHistory_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _NotificationCenter_Opening(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& args);
        void _NotificationHistory_ItemClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::ItemClickEventArgs& args);
        void _NotificationHistory_KeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& args);
        void _MarkAllNotificationsRead_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ClearAllNotificationHistory_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

    private:
        static constexpr double MinimumExpandedWidth{ 152.0 };
        static constexpr double MaximumExpandedWidth{ 240.0 };
        static constexpr double DefaultExpandedWidth{ 176.0 };
        static constexpr double PreviousDefaultExpandedWidth{ 200.0 };
        static constexpr double LegacyDefaultExpandedWidth{ 280.0 };
        static constexpr double CollapsedWidth{ 0.0 };

        bool _isExpanded{ true };
        bool _isWindowNarrow{ false };
        bool _layoutElementsReady{ false };
        bool _layoutAnimationsEnabled{ false };
        double _expandedWidth{ DefaultExpandedWidth };
        Windows::UI::Xaml::Media::Animation::Storyboard _layoutStoryboard{ nullptr };
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> _notificationHistory;

        void _ApplyLayout();
        void _SetExpandedContentVisibility(Windows::UI::Xaml::Visibility visibility);
        void _ActivateNotification(const TerminalApp::WorkspaceNotification& notification);
        void _SetWorkspaceItemCloseButtonVisibility(const Windows::Foundation::IInspectable& sender, Windows::UI::Xaml::Visibility visibility) const;
        TerminalApp::WorkspaceItemViewModel _ItemFromSender(const Windows::Foundation::IInspectable& sender) const;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(WorkspaceSidebar);
}
