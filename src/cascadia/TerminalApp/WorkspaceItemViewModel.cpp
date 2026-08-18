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

            if (const auto status = _tab.TabStatus())
            {
                _tabStatusChangedRevoker = status.PropertyChanged(winrt::auto_revoke, [weakThis{ get_weak() }](const auto&, const auto&) {
                    if (const auto self = weakThis.get())
                    {
                        self->_UpdateStatus();
                    }
                });
            }
        }

        _UpdateStatus();
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
        for (auto index = _notifications.Size(); index > 0; --index)
        {
            const auto notification = _notifications.GetAt(index - 1);
            if (!notification.IsRead())
            {
                return notification;
            }
        }
        return nullptr;
    }

    uint32_t WorkspaceItemViewModel::NotificationHistoryCount() const noexcept
    {
        return _notifications.Size();
    }

    winrt::hstring WorkspaceItemViewModel::NotificationHistoryCountText() const
    {
        const auto count = NotificationHistoryCount();
        return count == 0 ? winrt::hstring{} : winrt::to_hstring(count);
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> WorkspaceItemViewModel::Notifications() const noexcept
    {
        return _notifications;
    }

    void WorkspaceItemViewModel::RestoreWorkspaceId(const winrt::guid& workspaceId) noexcept
    {
        if (workspaceId != winrt::guid{})
        {
            _workspaceId = workspaceId;
        }
    }

    void WorkspaceItemViewModel::AddNotification(const uint32_t paneId, const winrt::hstring& title, const winrt::hstring& message)
    {
        _notifications.Append(TerminalApp::WorkspaceNotification{ _workspaceId, paneId, Title(), title, message, winrt::clock::now() });
        if (_notifications.Size() > MaximumNotifications)
        {
            _notifications.RemoveAt(0);
        }
        _UpdateNotificationProperties();
    }

    void WorkspaceItemViewModel::MarkAllNotificationsRead()
    {
        bool changed = false;
        for (const auto& notification : _notifications)
        {
            if (!notification.IsRead())
            {
                notification.IsRead(true);
                changed = true;
            }
        }

        if (changed)
        {
            _UpdateNotificationProperties();
        }
    }

    void WorkspaceItemViewModel::MarkNotificationsReadForPane(const uint32_t paneId)
    {
        bool changed = false;
        for (const auto& notification : _notifications)
        {
            if (notification.PaneId() == paneId && !notification.IsRead())
            {
                notification.IsRead(true);
                changed = true;
            }
        }

        if (changed)
        {
            _UpdateNotificationProperties();
        }
    }

    void WorkspaceItemViewModel::ClearNotificationHistory()
    {
        if (_notifications.Size() == 0)
        {
            return;
        }

        _notifications.Clear();
        _UpdateNotificationProperties();
    }

    void WorkspaceItemViewModel::_UpdateNotificationProperties()
    {
        uint32_t unreadCount = 0;
        for (const auto& notification : _notifications)
        {
            unreadCount += notification.IsRead() ? 0 : 1;
        }
        UnreadCount(unreadCount);

        const auto latest = LatestUnreadNotification();
        LatestNotification(latest ? (latest.Message().empty() ? latest.Title() : latest.Message()) : winrt::hstring{});

        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"UnreadBadgeText" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"NotificationAutomationName" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"NotificationHistoryCount" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"NotificationHistoryCountText" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"LatestUnreadNotification" });
        _UpdateStatus();
    }

    void WorkspaceItemViewModel::_UpdateStatus()
    {
        winrt::hstring text;
        if (const auto status = _tab ? _tab.TabStatus() : nullptr)
        {
            if (status.IsConnectionClosed())
            {
                text = RS_(L"WorkspaceSidebar_StatusDisconnected");
            }
            else if (UnreadCount() > 0)
            {
                text = RS_(L"WorkspaceSidebar_StatusAttention");
            }
            else
            {
                switch (status.ProgressState())
                {
                case 1:
                    text = status.ProgressValue() > 0 ?
                               winrt::hstring{ RS_fmt(L"WorkspaceSidebar_StatusProgress", status.ProgressValue()) } :
                               RS_(L"WorkspaceSidebar_StatusRunning");
                    break;
                case 2:
                    text = RS_(L"WorkspaceSidebar_StatusFailed");
                    break;
                case 3:
                    text = RS_(L"WorkspaceSidebar_StatusRunning");
                    break;
                case 4:
                    text = RS_(L"WorkspaceSidebar_StatusPaused");
                    break;
                default:
                    if (status.IsInputBroadcastActive())
                    {
                        text = RS_(L"WorkspaceSidebar_StatusBroadcasting");
                    }
                    else if (status.IsReadOnlyActive())
                    {
                        text = RS_(L"WorkspaceSidebar_StatusReadOnly");
                    }
                    break;
                }
            }
        }
        StatusText(text);
    }
}
