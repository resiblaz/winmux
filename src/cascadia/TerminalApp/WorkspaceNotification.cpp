// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceNotification.h"

#include "WorkspaceNotification.g.cpp"

namespace winrt::TerminalApp::implementation
{
    WorkspaceNotification::WorkspaceNotification(const winrt::guid& workspaceId,
                                                 const uint32_t paneId,
                                                 const winrt::hstring& title,
                                                 const winrt::hstring& message,
                                                 const Windows::Foundation::DateTime& timestamp) :
        _workspaceId{ workspaceId },
        _paneId{ paneId },
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

    uint32_t WorkspaceNotification::PaneId() const noexcept
    {
        return _paneId;
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
}
