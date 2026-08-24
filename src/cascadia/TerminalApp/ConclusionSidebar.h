// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ConclusionSidebar.g.h"
#include "AgentHistoryStore.h"

namespace winrt::TerminalApp::implementation
{
    struct ConclusionSidebar : ConclusionSidebarT<ConclusionSidebar>
    {
        static constexpr double NarrowWindowThreshold{ 1040.0 };

        ConclusionSidebar();

        Windows::Foundation::Collections::IObservableVector<TerminalApp::AgentHistoryItem> HistoryItems() const noexcept;

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
        bool AddPrompt(const winrt::guid& workspaceId,
                       const winrt::hstring& workspaceTitle,
                       const winrt::hstring& provider,
                       const winrt::hstring& sessionId,
                       const winrt::hstring& turnId,
                       const winrt::hstring& cwd,
                       const winrt::hstring& prompt,
                       const winrt::hstring& model,
                       const Windows::Foundation::DateTime& timestamp,
                       std::optional<AgentHistoryEventReceipt> event = std::nullopt);
        bool AddConclusion(const winrt::guid& workspaceId,
                           const winrt::hstring& workspaceTitle,
                           const winrt::hstring& provider,
                           const winrt::hstring& sessionId,
                           const winrt::hstring& turnId,
                           const winrt::hstring& cwd,
                           const winrt::hstring& title,
                           const winrt::hstring& prompt,
                           const winrt::hstring& finalMarkdown,
                           const winrt::hstring& model,
                           const Windows::Foundation::DateTime& timestamp,
                           std::optional<AgentHistoryEventReceipt> event = std::nullopt);
        void RefreshHistory();

        void _Toggle_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Scope_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Search_TextChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Pin_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Copy_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Delete_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

    private:
        static constexpr double ExpandedWidth{ 320.0 };
        static constexpr double CollapsedWidth{ 48.0 };
        static constexpr uint32_t MaximumHistory{ 1000 };

        bool _isExpanded{ true };
        bool _isWindowNarrow{ false };
        bool _isInitialized{ false };
        bool _showAllWorkspaces{ false };
        winrt::guid _activeWorkspaceId{};
        winrt::hstring _activeWorkspaceTitle;
        AgentHistoryStore _historyStore;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::AgentHistoryItem> _historyItems;
        std::unordered_map<std::wstring, Windows::UI::Xaml::Controls::PivotItem> _toolTabs;

        void _ApplyLayout();
        void _RefreshHistory();
        void _UpdateContentState();
        TerminalApp::AgentHistoryItem _ItemFromSender(const Windows::Foundation::IInspectable& sender) const;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(ConclusionSidebar);
}
