// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceItemViewModel.h"

#include "WorkspaceItemViewModel.g.cpp"

namespace winrt::TerminalApp::implementation
{
    WorkspaceItemViewModel::WorkspaceItemViewModel(const TerminalApp::Tab& tab) :
        _tab{ tab },
        _notifications{ winrt::single_threaded_observable_vector<TerminalApp::WorkspaceNotification>() }
    {
        GUID id{};
        THROW_IF_FAILED(CoCreateGuid(&id));
        _workspaceId = id;

        if (_tab)
        {
            _tabChangedRevoker = _tab.PropertyChanged(winrt::auto_revoke, [weakThis{ get_weak() }](const auto&, const auto& args) {
                if (const auto self = weakThis.get())
                {
                    self->PropertyChanged.raise(*self, args);
                    if (args.PropertyName() == L"IsPinned")
                    {
                        self->PropertyChanged.raise(*self, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"PinAutomationName" });
                    }
                }
            });
        }
    }

    winrt::guid WorkspaceItemViewModel::WorkspaceId() const noexcept
    {
        return _workspaceId;
    }

    TerminalApp::Tab WorkspaceItemViewModel::SourceTab() const noexcept
    {
        return _tab;
    }

    winrt::hstring WorkspaceItemViewModel::Title() const
    {
        return _tab ? _tab.Title() : winrt::hstring{};
    }

    winrt::hstring WorkspaceItemViewModel::Icon() const
    {
        return _tab ? _tab.Icon() : winrt::hstring{};
    }

    bool WorkspaceItemViewModel::IsPinned() const
    {
        return _tab && _tab.IsPinned();
    }

    void WorkspaceItemViewModel::IsPinned(const bool value)
    {
        if (_tab)
        {
            _tab.IsPinned(value);
        }
    }

    winrt::hstring WorkspaceItemViewModel::PinIconGlyph() const
    {
        return _tab ? _tab.PinIconGlyph() : winrt::hstring{};
    }

    winrt::hstring WorkspaceItemViewModel::PinActionText() const
    {
        return IsPinned() ? RS_(L"WorkspaceSidebar_UnpinAction") : RS_(L"WorkspaceSidebar_PinAction");
    }

    winrt::hstring WorkspaceItemViewModel::PinAutomationName() const
    {
        return IsPinned() ? RS_(L"WorkspaceSidebar_UnpinAutomationName") : RS_(L"WorkspaceSidebar_PinAutomationName");
    }

    winrt::hstring WorkspaceItemViewModel::SwitchShortcutText() const
    {
        return _tab ? _tab.SwitchShortcutText() : winrt::hstring{};
    }

    winrt::hstring WorkspaceItemViewModel::UnreadBadgeText() const
    {
        const auto count = UnreadCount();
        return count == 0 ? winrt::hstring{} : winrt::to_hstring(count);
    }

    winrt::hstring WorkspaceItemViewModel::NotificationAutomationName() const
    {
        const auto count = UnreadCount();
        return count == 0 ? winrt::hstring{} : winrt::hstring{ RS_fmt(L"WorkspaceSidebar_UnreadAutomationName", count) };
    }

    TerminalApp::WorkspaceNotification WorkspaceItemViewModel::LatestUnreadNotification() const
    {
        const auto size = _notifications.Size();
        return size == 0 ? nullptr : _notifications.GetAt(size - 1);
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> WorkspaceItemViewModel::Notifications() const noexcept
    {
        return _notifications;
    }

    void WorkspaceItemViewModel::AddNotification(const uint32_t paneId, const winrt::hstring& title, const winrt::hstring& message)
    {
        _notifications.Append(TerminalApp::WorkspaceNotification{ _workspaceId, paneId, title, message, winrt::clock::now() });
        if (_notifications.Size() > MaximumNotifications)
        {
            _notifications.RemoveAt(0);
        }
        _UpdateNotificationProperties();
    }

    void WorkspaceItemViewModel::ClearNotifications()
    {
        if (_notifications.Size() == 0)
        {
            return;
        }

        _notifications.Clear();
        _UpdateNotificationProperties();
    }

    void WorkspaceItemViewModel::ClearNotificationsForPane(const uint32_t paneId)
    {
        bool changed = false;
        for (auto index = _notifications.Size(); index > 0; --index)
        {
            if (_notifications.GetAt(index - 1).PaneId() == paneId)
            {
                _notifications.RemoveAt(index - 1);
                changed = true;
            }
        }

        if (changed)
        {
            _UpdateNotificationProperties();
        }
    }

    void WorkspaceItemViewModel::_UpdateNotificationProperties()
    {
        const auto count = _notifications.Size();
        UnreadCount(count);

        const auto latest = LatestUnreadNotification();
        LatestNotification(latest ? (latest.Message().empty() ? latest.Title() : latest.Message()) : winrt::hstring{});

        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"UnreadBadgeText" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"NotificationAutomationName" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"LatestUnreadNotification" });
    }
}
