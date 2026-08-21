// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ConclusionSidebar.g.h"
#include "WorkspaceNotification.h"

namespace winrt::TerminalApp::implementation
{
    struct ConclusionSidebar : ConclusionSidebarT<ConclusionSidebar>
    {
        static constexpr double NarrowWindowThreshold{ 1040.0 };

        ConclusionSidebar();

        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> Conclusions() const noexcept;

        bool IsExpanded() const noexcept;
        void IsExpanded(bool value);

        bool RegisterToolTab(const winrt::hstring& id,
                             const winrt::hstring& title,
                             const winrt::hstring& iconGlyph,
                             const Windows::UI::Xaml::UIElement& content);
        bool RemoveToolTab(const winrt::hstring& id);
        bool SelectToolTab(const winrt::hstring& id);

        void SetWindowNarrow(bool value);
        void SetActiveWorkspace(const winrt::guid& workspaceId, const winrt::hstring& workspaceTitle);
        void AddConclusion(const winrt::guid& workspaceId,
                           const winrt::hstring& workspaceTitle,
                           const winrt::hstring& title,
                           const winrt::hstring& message,
                           const Windows::Foundation::DateTime& timestamp);

        void _Toggle_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

    private:
        static constexpr double ExpandedWidth{ 320.0 };
        static constexpr double CollapsedWidth{ 48.0 };
        static constexpr uint32_t MaximumHistory{ 100 };

        bool _isExpanded{ true };
        bool _isWindowNarrow{ false };
        winrt::guid _activeWorkspaceId{};
        winrt::hstring _activeWorkspaceTitle;
        std::vector<TerminalApp::WorkspaceNotification> _history;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> _conclusions;
        std::unordered_map<std::wstring, Windows::UI::Xaml::Controls::PivotItem> _toolTabs;

        void _ApplyLayout();
        void _RebuildVisibleConclusions();
        void _UpdateContentState();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(ConclusionSidebar);
}
