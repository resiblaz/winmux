// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceNotification.g.h"

namespace winrt::TerminalApp::implementation
{
    struct WorkspaceNotification : WorkspaceNotificationT<WorkspaceNotification>
    {
        WorkspaceNotification(const winrt::guid& workspaceId,
                              uint32_t paneId,
                              const winrt::hstring& workspaceTitle,
                              const winrt::hstring& title,
                              const winrt::hstring& message,
                              const Windows::Foundation::DateTime& timestamp);

        winrt::guid NotificationId() const noexcept;
        winrt::guid WorkspaceId() const noexcept;
        uint32_t PaneId() const noexcept;
        winrt::hstring WorkspaceTitle() const;
        winrt::hstring Title() const;
        winrt::hstring Message() const;
        Windows::Foundation::DateTime Timestamp() const noexcept;
        winrt::hstring TimestampText() const;
        winrt::hstring AutomationName() const;
        bool IsRead() const noexcept;
        void IsRead(bool value) noexcept;

    private:
        winrt::guid _notificationId{};
        winrt::guid _workspaceId{};
        uint32_t _paneId{};
        winrt::hstring _workspaceTitle;
        winrt::hstring _title;
        winrt::hstring _message;
        Windows::Foundation::DateTime _timestamp{};
        bool _isRead{ false };
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(WorkspaceNotification);
}
