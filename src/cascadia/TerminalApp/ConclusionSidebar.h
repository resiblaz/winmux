// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ConclusionSidebar.g.h"
#include "AgentHistoryStore.h"

#include <functional>

namespace winrt::TerminalApp::implementation
{
    enum class CodexHistoryUiState
    {
        Unavailable,
        Ready,
        Syncing,
        Loading,
        Resuming,
        Error,
    };

    struct ConclusionSidebar : ConclusionSidebarT<ConclusionSidebar>
    {
        using CodexHistorySyncHandler = std::function<void(const winrt::guid& workspaceId,
                                                           const winrt::hstring& workspaceTitle,
                                                           const winrt::hstring& cwd)>;
        using CodexHistorySessionHandler = std::function<void(const winrt::guid& workspaceId,
                                                              const winrt::hstring& workspaceTitle,
                                                              const winrt::hstring& cwd,
                                                              const winrt::hstring& sessionId)>;

        static constexpr double NarrowWindowThreshold{ 1040.0 };

        ConclusionSidebar();

        Windows::Foundation::Collections::IObservableVector<TerminalApp::AgentHistoryItem> HistoryItems() const noexcept;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::AgentHistoryItem> SessionItems() const noexcept;

        bool IsExpanded() const noexcept;
        void IsExpanded(bool value);

        bool RegisterToolTab(const winrt::hstring& id,
                             const winrt::hstring& title,
                             const winrt::hstring& iconGlyph,
                             const Windows::UI::Xaml::UIElement& content);
        bool RemoveToolTab(const winrt::hstring& id);
        bool SelectToolTab(const winrt::hstring& id);

        void SetWindowNarrow(bool value);
        void SetActiveWorkspace(const winrt::guid& workspaceId,
                                const winrt::hstring& workspaceTitle,
                                const winrt::hstring& cwd = {});
        void SetCodexHistoryHandlers(CodexHistorySyncHandler syncHandler,
                                     CodexHistorySessionHandler loadHandler,
                                     CodexHistorySessionHandler resumeHandler);
        void SetCodexOperationState(bool busy, CodexHistoryUiState state);
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
        void RefreshSessions();

        void _Toggle_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Scope_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Search_TextChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _SessionFilter_TextChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _DateFilter_SelectionChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);
        void _SessionSearch_TextChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _SessionSelection_Changed(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);
        void _SyncSessions_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _LoadSession_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ResumeSession_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Pin_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Copy_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Delete_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        safe_void_coroutine _ClearHistory_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

    private:
        static constexpr double ExpandedWidth{ 320.0 };
        static constexpr double CollapsedWidth{ 48.0 };
        static constexpr uint32_t MaximumHistory{ 1000 };
        static constexpr uint32_t MaximumSessions{ 1000 };

        bool _isExpanded{ true };
        bool _isWindowNarrow{ false };
        bool _isInitialized{ false };
        bool _showAllWorkspaces{ false };
        bool _codexOperationBusy{ false };
        bool _historyClearInFlight{ false };
        bool _historyAvailable{ false };
        uint64_t _historyQueryGeneration{};
        uint64_t _sessionQueryGeneration{};
        SafeDispatcherTimer _historySearchTimer;
        SafeDispatcherTimer _sessionSearchTimer;
        CodexHistoryUiState _codexUiState{ CodexHistoryUiState::Unavailable };
        winrt::guid _activeWorkspaceId{};
        winrt::hstring _activeWorkspaceTitle;
        winrt::hstring _activeWorkspaceCwd;
        AgentHistoryStore _historyStore;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::AgentHistoryItem> _historyItems;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::AgentHistoryItem> _sessionItems;
        CodexHistorySyncHandler _codexSyncHandler;
        CodexHistorySessionHandler _codexLoadHandler;
        CodexHistorySessionHandler _codexResumeHandler;
        std::unordered_map<std::wstring, Windows::UI::Xaml::Controls::PivotItem> _toolTabs;

        void _ApplyLayout();
        void _QueueHistoryRefresh();
        void _QueueSessionRefresh();
        safe_void_coroutine _RefreshHistory();
        safe_void_coroutine _RefreshSessions();
        void _UpdateContentState();
        void _UpdateSessionActions();
        TerminalApp::AgentHistoryItem _SelectedSession();
        TerminalApp::AgentHistoryItem _ItemFromSender(const Windows::Foundation::IInspectable& sender) const;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(ConclusionSidebar);
}
