// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentHistoryItem.h"

#include "AgentHistoryItem.g.cpp"

namespace winrt::TerminalApp::implementation
{
    winrt::hstring AgentHistoryItem::TimestampText() const
    {
        const auto ticks = _Timestamp.time_since_epoch().count();
        if (ticks <= 0)
        {
            return {};
        }

        ULARGE_INTEGER value{};
        value.QuadPart = static_cast<ULONGLONG>(ticks);
        const FILETIME fileTime{ value.LowPart, value.HighPart };
        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        if (FileTimeToSystemTime(&fileTime, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        {
            return winrt::hstring{ fmt::format(FMT_COMPILE(L"{:04}-{:02}-{:02} {:02}:{:02}"),
                                               local.wYear,
                                               local.wMonth,
                                               local.wDay,
                                               local.wHour,
                                               local.wMinute) };
        }

        return {};
    }

    winrt::hstring AgentHistoryItem::SessionLabel() const
    {
        constexpr size_t shortIdLength{ 8 };
        const std::wstring_view sessionId{ _SessionId };
        const auto shortId = sessionId.substr(0, std::min(sessionId.size(), shortIdLength));

        if (_Provider.empty())
        {
            return winrt::hstring{ shortId };
        }
        if (shortId.empty())
        {
            return _Provider;
        }

        std::wstring label{ _Provider };
        label.append(L" · ");
        label.append(shortId);
        return winrt::hstring{ label };
    }

    bool AgentHistoryItem::HasPrompt() const noexcept
    {
        return !_Prompt.empty();
    }

    winrt::hstring AgentHistoryItem::AutomationName() const
    {
        std::wstring value;
        const auto append = [&](const winrt::hstring& detail) {
            if (!detail.empty())
            {
                if (!value.empty())
                {
                    value.append(L", ");
                }
                value.append(detail);
            }
        };

        append(_Title);
        append(_WorkspaceTitle);
        append(SessionLabel());
        append(TimestampText());
        return winrt::hstring{ value };
    }
}
