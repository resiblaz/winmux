// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceSidebar.h"

#include "WorkspaceSidebar.g.cpp"

using namespace winrt;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    WorkspaceSidebar::WorkspaceSidebar() :
        _notificationHistory{ winrt::single_threaded_observable_vector<TerminalApp::WorkspaceNotification>() }
    {
        InitializeComponent();

        const auto state = ApplicationState::SharedInstance();
        _expandedWidth = std::clamp(state.WorkspaceSidebarWidth(), MinimumExpandedWidth, MaximumExpandedWidth);
        _isExpanded = state.WorkspaceSidebarExpanded();
        _ApplyLayout();
    }

    bool WorkspaceSidebar::IsExpanded() const noexcept
    {
        return _isExpanded;
    }

    void WorkspaceSidebar::IsExpanded(const bool value)
    {
        if (_isExpanded == value)
        {
            return;
        }

        _isExpanded = value;
        _ApplyLayout();
        ApplicationState::SharedInstance().WorkspaceSidebarExpanded(value);
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsExpanded" });
    }

    double WorkspaceSidebar::ExpandedWidth() const noexcept
    {
        return _expandedWidth;
    }

    void WorkspaceSidebar::ExpandedWidth(const double value)
    {
        const auto clamped = std::clamp(value, MinimumExpandedWidth, MaximumExpandedWidth);
        if (_expandedWidth == clamped)
        {
            return;
        }

        _expandedWidth = clamped;
        if (_isExpanded)
        {
            Width(_expandedWidth);
        }
        ApplicationState::SharedInstance().WorkspaceSidebarWidth(clamped);
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"ExpandedWidth" });
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> WorkspaceSidebar::NotificationHistory() const noexcept
    {
        return _notificationHistory;
    }

    void WorkspaceSidebar::RefreshNotifications()
    {
        std::vector<TerminalApp::WorkspaceNotification> history;
        uint32_t unreadCount = 0;

        if (const auto workspaces = WorkspaceList().ItemsSource().try_as<Windows::Foundation::Collections::IIterable<IInspectable>>())
        {
            for (const auto& value : workspaces)
            {
                if (const auto item = value.try_as<TerminalApp::WorkspaceItemViewModel>())
                {
                    unreadCount += item.UnreadCount();
                    for (const auto& notification : item.Notifications())
                    {
                        history.emplace_back(notification);
                    }
                }
            }
        }

        std::sort(history.begin(), history.end(), [](const auto& left, const auto& right) {
            return left.Timestamp() > right.Timestamp();
        });

        _notificationHistory.Clear();
        for (const auto& notification : history)
        {
            _notificationHistory.Append(notification);
        }

        const auto hasHistory = !history.empty();
        NotificationCenterBadgeText().Text(unreadCount == 0 ? winrt::hstring{} : winrt::to_hstring(unreadCount));
        NotificationCenterBadge().Visibility(unreadCount == 0 ? Visibility::Collapsed : Visibility::Visible);
        NotificationHistoryList().Visibility(hasHistory ? Visibility::Visible : Visibility::Collapsed);
        NotificationCenterEmptyState().Visibility(hasHistory ? Visibility::Collapsed : Visibility::Visible);
        MarkAllNotificationsReadButton().IsEnabled(unreadCount > 0);
        ClearAllNotificationHistoryButton().IsEnabled(hasHistory);

        const auto automationName = unreadCount == 0 ?
                                        RS_(L"WorkspaceSidebar_NotificationCenterAutomationName") :
                                        winrt::hstring{ RS_fmt(L"WorkspaceSidebar_NotificationCenterUnreadAutomationName", unreadCount) };
        Automation::AutomationProperties::SetName(NotificationCenterButton(), automationName);
    }

    void WorkspaceSidebar::_ApplyLayout()
    {
        Width(_isExpanded ? _expandedWidth : CollapsedWidth);
        ResizeHandle().Visibility(_isExpanded ? Visibility::Visible : Visibility::Collapsed);
        SidebarTitle().Visibility(_isExpanded ? Visibility::Visible : Visibility::Collapsed);
        NotificationCenterButton().Visibility(_isExpanded ? Visibility::Visible : Visibility::Collapsed);
        NewButtonText().Visibility(_isExpanded ? Visibility::Visible : Visibility::Collapsed);
        ToggleButton().Content(box_value(_isExpanded ? L"\uE76B" : L"\uE76C"));
        Automation::AutomationProperties::SetName(ToggleButton(), _isExpanded ? RS_(L"WorkspaceSidebar_CollapseAutomationName") : RS_(L"WorkspaceSidebar_ExpandAutomationName"));

        const auto templateKey = _isExpanded ? L"ExpandedWorkspaceTemplate" : L"CollapsedWorkspaceTemplate";
        const auto itemStyleKey = _isExpanded ? L"ExpandedWorkspaceItemContainerStyle" : L"CollapsedWorkspaceItemContainerStyle";
        WorkspaceList().ItemTemplate(Resources().Lookup(box_value(templateKey)).as<DataTemplate>());
        WorkspaceList().ItemContainerStyle(Resources().Lookup(box_value(itemStyleKey)).as<Windows::UI::Xaml::Style>());
    }

    TerminalApp::WorkspaceItemViewModel WorkspaceSidebar::_ItemFromSender(const IInspectable& sender) const
    {
        if (const auto element = sender.try_as<FrameworkElement>())
        {
            return element.Tag().try_as<TerminalApp::WorkspaceItemViewModel>();
        }
        return nullptr;
    }

    void WorkspaceSidebar::_Toggle_Click(const IInspectable&, const RoutedEventArgs&)
    {
        IsExpanded(!_isExpanded);
    }

    void WorkspaceSidebar::_Resize_DragDelta(const IInspectable&, const Controls::Primitives::DragDeltaEventArgs& args)
    {
        if (_isExpanded)
        {
            ExpandedWidth(_expandedWidth + args.HorizontalChange());
        }
    }

    void WorkspaceSidebar::_New_Click(const IInspectable&, const RoutedEventArgs&)
    {
        NewRequested.raise(*this, nullptr);
    }

    void WorkspaceSidebar::_Pin_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            PinRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_Close_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            CloseRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_CloseOthers_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            CloseOthersRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_Rename_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            RenameRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_ConfigureAgentResume_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ConfigureAgentResumeRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_ResumeAgent_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ResumeAgentRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_ClearAgentResume_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ClearAgentResumeRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_Notification_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ActivateNotificationRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_MarkNotificationsRead_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            MarkNotificationsReadRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_ClearNotificationHistory_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ClearNotificationHistoryRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_NotificationCenter_Opening(const IInspectable&, const IInspectable&)
    {
        RefreshNotifications();
    }

    void WorkspaceSidebar::_NotificationHistory_ItemClick(const IInspectable&, const ItemClickEventArgs& args)
    {
        if (const auto notification = args.ClickedItem().try_as<TerminalApp::WorkspaceNotification>())
        {
            NotificationCenterFlyout().Hide();
            ActivateNotificationItemRequested.raise(*this, notification);
        }
    }

    void WorkspaceSidebar::_MarkAllNotificationsRead_Click(const IInspectable&, const RoutedEventArgs&)
    {
        MarkAllNotificationsReadRequested.raise(*this, nullptr);
    }

    void WorkspaceSidebar::_ClearAllNotificationHistory_Click(const IInspectable&, const RoutedEventArgs&)
    {
        ClearAllNotificationHistoryRequested.raise(*this, nullptr);
    }
}
