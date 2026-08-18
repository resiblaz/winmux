// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceItemViewModel.h"

#include "WorkspaceItemViewModel.g.cpp"

namespace winrt::TerminalApp::implementation
{
    WorkspaceItemViewModel::WorkspaceItemViewModel(const TerminalApp::Tab& tab) :
        _tab{ tab }
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
}
