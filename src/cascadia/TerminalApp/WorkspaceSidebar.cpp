// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceSidebar.h"
#include "WorkspaceItemViewModel.h"

#include "WorkspaceSidebar.g.cpp"

using namespace winrt;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media::Animation;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr uint32_t MaximumNotificationHistory{ 1000 };
    }

    WorkspaceSidebar::WorkspaceSidebar() :
        _notificationHistory{ winrt::single_threaded_observable_vector<TerminalApp::WorkspaceNotification>() }
    {
        InitializeComponent();

        const auto state = ApplicationState::SharedInstance();
        const auto savedWidth = state.WorkspaceSidebarWidth();
        const auto usesPreviousDefault = savedWidth == PreviousDefaultExpandedWidth || savedWidth == LegacyDefaultExpandedWidth;
        _expandedWidth = std::clamp(usesPreviousDefault ? DefaultExpandedWidth : savedWidth,
                                    MinimumExpandedWidth,
                                    MaximumExpandedWidth);
        if (usesPreviousDefault)
        {
            state.WorkspaceSidebarWidth(_expandedWidth);
        }
        _isExpanded = state.WorkspaceSidebarExpanded();
        _ApplyLayout();

        Loaded([weakThis{ get_weak() }](auto&&, auto&&) {
            if (const auto self = weakThis.get())
            {
                self->_layoutElementsReady = true;
                self->_ApplyLayout();
                self->_layoutAnimationsEnabled = true;
            }
        });
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
        if (IsEffectivelyExpanded())
        {
            Width(_expandedWidth);
        }
        ApplicationState::SharedInstance().WorkspaceSidebarWidth(clamped);
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"ExpandedWidth" });
    }

    void WorkspaceSidebar::SetWindowNarrow(const bool value)
    {
        if (_isWindowNarrow != value)
        {
            _isWindowNarrow = value;
            _ApplyLayout();
        }
    }

    bool WorkspaceSidebar::IsWindowNarrow() const noexcept
    {
        return _isWindowNarrow;
    }

    bool WorkspaceSidebar::IsEffectivelyExpanded() const noexcept
    {
        return _isExpanded && !_isWindowNarrow;
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
        if (history.size() > MaximumNotificationHistory)
        {
            history.erase(history.begin() + MaximumNotificationHistory, history.end());
        }

        _notificationHistory.ReplaceAll({ history.data(), gsl::narrow_cast<uint32_t>(history.size()) });

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

    void WorkspaceSidebar::OpenNotificationCenter(const FrameworkElement& target)
    {
        RefreshNotifications();
        NotificationCenterFlyout().ShowAt(target);
    }

    void WorkspaceSidebar::ShowSwitchHints(const bool value)
    {
        if (const auto workspaces = WorkspaceList().ItemsSource().try_as<Windows::Foundation::Collections::IIterable<IInspectable>>())
        {
            for (const auto& workspace : workspaces)
            {
                if (const auto item = workspace.try_as<TerminalApp::WorkspaceItemViewModel>())
                {
                    winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item)->ShowSwitchShortcutHint(value);
                }
            }
        }
    }

    void WorkspaceSidebar::_SetExpandedContentVisibility(const Windows::UI::Xaml::Visibility visibility)
    {
        SidebarTitle().Visibility(visibility);
        NotificationCenterButton().Visibility(visibility);
        WorkspaceList().Visibility(visibility);
    }

    void WorkspaceSidebar::_ApplyLayout()
    {
        const auto expanded = IsEffectivelyExpanded();
        const auto targetWidth = expanded ? _expandedWidth : CollapsedWidth;
        const auto targetOpacity = expanded ? 1.0 : 0.0;

        // When this control is created from another XAML file, its constructor can
        // run before the named child elements have been connected. The outer width
        // is safe to initialize immediately; child state is applied once Loaded.
        if (!_layoutElementsReady)
        {
            Width(targetWidth);
            return;
        }

        ResizeHandle().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
        _SetExpandedContentVisibility(expanded ? Visibility::Visible : Visibility::Collapsed);

        if (_layoutAnimationsEnabled)
        {
            const auto startWidth = ActualWidth();
            const auto startOpacity = SidebarChrome().Opacity();
            if (_layoutStoryboard)
            {
                _layoutStoryboard.Stop();
            }

            Width(targetWidth);
            SidebarChrome().Opacity(targetOpacity);

            const auto duration = DurationHelper::FromTimeSpan(Windows::Foundation::TimeSpan{ std::chrono::milliseconds{ 150 } });
            _layoutStoryboard = Storyboard{};
            _layoutStoryboard.Duration(duration);

            DoubleAnimation widthAnimation{};
            widthAnimation.Duration(duration);
            widthAnimation.From(startWidth);
            widthAnimation.To(targetWidth);
            widthAnimation.EasingFunction(QuadraticEase{});
            widthAnimation.EnableDependentAnimation(true);
            widthAnimation.FillBehavior(FillBehavior::Stop);
            _layoutStoryboard.Children().Append(widthAnimation);
            _layoutStoryboard.SetTarget(widthAnimation, *this);
            _layoutStoryboard.SetTargetProperty(widthAnimation, L"Width");

            DoubleAnimation opacityAnimation{};
            opacityAnimation.Duration(DurationHelper::FromTimeSpan(Windows::Foundation::TimeSpan{ std::chrono::milliseconds{ 90 } }));
            opacityAnimation.From(startOpacity);
            opacityAnimation.To(targetOpacity);
            opacityAnimation.FillBehavior(FillBehavior::Stop);
            _layoutStoryboard.Children().Append(opacityAnimation);
            _layoutStoryboard.SetTarget(opacityAnimation, SidebarChrome());
            _layoutStoryboard.SetTargetProperty(opacityAnimation, L"Opacity");

            _layoutStoryboard.Begin();
        }
        else
        {
            Width(targetWidth);
            SidebarChrome().Opacity(targetOpacity);
            _SetExpandedContentVisibility(expanded ? Visibility::Visible : Visibility::Collapsed);
        }

        ToggleButton().Content(box_value(expanded ? L"\uE76B" : L"\uE76C"));
        ToggleButton().IsEnabled(!_isWindowNarrow);
        Automation::AutomationProperties::SetName(ToggleButton(),
                                                   _isWindowNarrow ? RS_(L"WorkspaceSidebar_NarrowAutomationName") :
                                                                     expanded ? RS_(L"WorkspaceSidebar_CollapseAutomationName") :
                                                                                RS_(L"WorkspaceSidebar_ExpandAutomationName"));
        Automation::AutomationProperties::SetHelpText(ToggleButton(), _isWindowNarrow ? RS_(L"WorkspaceSidebar_NarrowHelpText") : winrt::hstring{});

        WorkspaceList().ItemTemplate(Resources().Lookup(box_value(L"ExpandedWorkspaceTemplate")).as<DataTemplate>());
        WorkspaceList().ItemContainerStyle(Resources().Lookup(box_value(L"ExpandedWorkspaceItemContainerStyle")).as<Windows::UI::Xaml::Style>());
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
        if (IsEffectivelyExpanded())
        {
            ExpandedWidth(_expandedWidth + args.HorizontalChange());
        }
    }

    void WorkspaceSidebar::_Resize_PreviewKeyDown(const IInspectable&, const Input::KeyRoutedEventArgs& args)
    {
        switch (args.OriginalKey())
        {
        case Windows::System::VirtualKey::Left:
            ExpandedWidth(_expandedWidth - 8.0);
            break;
        case Windows::System::VirtualKey::Right:
            ExpandedWidth(_expandedWidth + 8.0);
            break;
        case Windows::System::VirtualKey::Home:
            ExpandedWidth(MinimumExpandedWidth);
            break;
        case Windows::System::VirtualKey::End:
            ExpandedWidth(MaximumExpandedWidth);
            break;
        default:
            return;
        }
        args.Handled(true);
    }

    void WorkspaceSidebar::_WorkspaceList_KeyDown(const IInspectable&, const Input::KeyRoutedEventArgs& args)
    {
        const auto item = WorkspaceList().SelectedItem().try_as<TerminalApp::WorkspaceItemViewModel>();
        if (!item)
        {
            return;
        }

        switch (args.OriginalKey())
        {
        case Windows::System::VirtualKey::F2:
            RenameRequested.raise(*this, item);
            break;
        case Windows::System::VirtualKey::Delete:
            CloseRequested.raise(*this, item);
            break;
        default:
            return;
        }
        args.Handled(true);
    }

    void WorkspaceSidebar::_SetWorkspaceItemCloseButtonVisibility(const IInspectable& sender, const Windows::UI::Xaml::Visibility visibility) const
    {
        if (const auto itemRoot = sender.try_as<FrameworkElement>())
        {
            if (const auto closeButton = itemRoot.FindName(L"WorkspaceItemCloseButton").try_as<Button>())
            {
                closeButton.Visibility(visibility);
            }
        }
    }

    void WorkspaceSidebar::_WorkspaceItem_PointerEntered(const IInspectable& sender, const Input::PointerRoutedEventArgs&)
    {
        _SetWorkspaceItemCloseButtonVisibility(sender, Visibility::Visible);
    }

    void WorkspaceSidebar::_WorkspaceItem_PointerExited(const IInspectable& sender, const Input::PointerRoutedEventArgs&)
    {
        _SetWorkspaceItemCloseButtonVisibility(sender, Visibility::Collapsed);
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
            _ActivateNotification(notification);
        }
    }

    void WorkspaceSidebar::_NotificationHistory_KeyDown(const IInspectable&, const Input::KeyRoutedEventArgs& args)
    {
        const auto key = args.OriginalKey();
        if (key != Windows::System::VirtualKey::Enter && key != Windows::System::VirtualKey::Space)
        {
            return;
        }

        if (const auto element = args.OriginalSource().try_as<FrameworkElement>())
        {
            if (const auto notification = element.DataContext().try_as<TerminalApp::WorkspaceNotification>())
            {
                _ActivateNotification(notification);
                args.Handled(true);
            }
        }
    }

    void WorkspaceSidebar::_ActivateNotification(const TerminalApp::WorkspaceNotification& notification)
    {
        NotificationCenterFlyout().Hide();
        ActivateNotificationItemRequested.raise(*this, notification);
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
