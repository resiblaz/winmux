// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "AgentHistoryItem.g.h"

namespace winrt::TerminalApp::implementation
{
    struct AgentHistoryItem : AgentHistoryItemT<AgentHistoryItem>
    {
        AgentHistoryItem() = default;

        WINRT_PROPERTY(winrt::guid, WorkspaceId);
        WINRT_PROPERTY(winrt::hstring, WorkspaceTitle);
        WINRT_PROPERTY(winrt::hstring, Provider);
        WINRT_PROPERTY(winrt::hstring, SessionId);
        WINRT_PROPERTY(winrt::hstring, TurnId);
        WINRT_PROPERTY(winrt::hstring, Cwd);
        WINRT_PROPERTY(winrt::hstring, Title);
        WINRT_PROPERTY(winrt::hstring, Prompt);
        WINRT_PROPERTY(winrt::hstring, Message);
        WINRT_PROPERTY(winrt::hstring, Model);
        WINRT_PROPERTY(Windows::Foundation::DateTime, Timestamp);
        WINRT_PROPERTY(bool, IsPinned, false);

    public:
        winrt::hstring TimestampText() const;
        winrt::hstring SessionLabel() const;
        bool HasPrompt() const noexcept;
        winrt::hstring AutomationName() const;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(AgentHistoryItem);
}
