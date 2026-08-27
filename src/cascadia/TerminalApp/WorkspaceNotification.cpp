// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceNotification.h"

#include "WorkspaceNotification.g.cpp"

namespace winrt::TerminalApp::implementation
{
    WorkspaceNotification::WorkspaceNotification(const winrt::guid& workspaceId,
                                                 const winrt::guid& surfaceId,
                                                 const uint32_t paneId,
                                                 const winrt::hstring& workspaceTitle,
                                                 const winrt::hstring& title,
                                                 const winrt::hstring& message,
                                                 const Windows::Foundation::DateTime& timestamp) :
        _workspaceId{ workspaceId },
        _surfaceId{ surfaceId },
        _paneId{ paneId },
        _workspaceTitle{ workspaceTitle },
        _title{ title },
        _message{ message },
        _timestamp{ timestamp }
    {
        GUID id{};
        THROW_IF_FAILED(CoCreateGuid(&id));
        _notificationId = id;
    }

    winrt::guid WorkspaceNotification::NotificationId() const noexcept
    {
        return _notificationId;
    }

    winrt::guid WorkspaceNotification::WorkspaceId() const noexcept
    {
        return _workspaceId;
    }

    winrt::guid WorkspaceNotification::SurfaceId() const noexcept
    {
        return _surfaceId;
    }

    uint32_t WorkspaceNotification::PaneId() const noexcept
    {
        return _paneId;
    }

    winrt::hstring WorkspaceNotification::WorkspaceTitle() const
    {
        return _workspaceTitle;
    }

    winrt::hstring WorkspaceNotification::Title() const
    {
        return _title;
    }

    winrt::hstring WorkspaceNotification::Message() const
    {
        return _message;
    }

    Windows::Foundation::DateTime WorkspaceNotification::Timestamp() const noexcept
    {
        return _timestamp;
    }

    winrt::hstring WorkspaceNotification::TimestampText() const
    {
        const auto ticks = _timestamp.time_since_epoch().count();
        const ULARGE_INTEGER value{ .QuadPart = gsl::narrow_cast<ULONGLONG>(ticks) };
        const FILETIME fileTime{ value.LowPart, value.HighPart };
        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        if (FileTimeToSystemTime(&fileTime, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        {
            return winrt::hstring{ fmt::format(FMT_COMPILE(L"{:02}:{:02}"), local.wHour, local.wMinute) };
        }
        return {};
    }

    winrt::hstring WorkspaceNotification::AutomationName() const
    {
        const auto readState = _isRead ? RS_(L"WorkspaceSidebar_NotificationReadAutomationState") : RS_(L"WorkspaceSidebar_NotificationUnreadAutomationState");
        return winrt::hstring{ RS_fmt(L"WorkspaceSidebar_NotificationItemAutomationName", _workspaceTitle, _title, _message, TimestampText(), readState) };
    }

    bool WorkspaceNotification::IsRead() const noexcept
    {
        return _isRead;
    }

    void WorkspaceNotification::IsRead(const bool value) noexcept
    {
        _isRead = value;
    }
}
