// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceSidebarRow.h"
#include "WorkspaceSidebarRow.g.cpp"
#include "WorkspaceSidebarRowTemplateSelector.g.cpp"

namespace winrt::TerminalApp::implementation
{
    Windows::UI::Xaml::DataTemplate WorkspaceSidebarRowTemplateSelector::SelectTemplateCore(
        const Windows::Foundation::IInspectable& item,
        const Windows::UI::Xaml::DependencyObject& /*container*/)
    {
        return SelectTemplateCore(item);
    }

    Windows::UI::Xaml::DataTemplate WorkspaceSidebarRowTemplateSelector::SelectTemplateCore(
        const Windows::Foundation::IInspectable& item)
    {
        if (const auto row = item.try_as<TerminalApp::WorkspaceSidebarRow>())
        {
            switch (row.Kind())
            {
            case TerminalApp::WorkspaceSidebarRowKind::Surface:
                return SurfaceTemplate();
            case TerminalApp::WorkspaceSidebarRowKind::Pane:
                return PaneTemplate();
            case TerminalApp::WorkspaceSidebarRowKind::Workspace:
            default:
                return WorkspaceTemplate();
            }
        }
        return WorkspaceTemplate();
    }
}
