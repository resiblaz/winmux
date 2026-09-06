// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "AgentCatalogPage.g.h"
#include "AgentEditorViewModel.h"

namespace winrt::TerminalApp::implementation
{
    struct AgentCatalogPage : AgentCatalogPageT<AgentCatalogPage>
    {
        AgentCatalogPage();
        void Open(std::shared_ptr<agents::IAgentCatalogSource> source, std::string workspace, std::wstring label,
                  winrt::TerminalApp::IDialogPresenter presenter, std::function<void()> closed);
        void Stop();
        void _Back_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _New_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _Refresh_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _More_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _ArchivedFilter_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _SearchChanged(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::Controls::TextChangedEventArgs&);
        void _AgentSelected(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs&);
        void _DraftChanged(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::Controls::TextChangedEventArgs&);
        void _Save_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _Copy_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _Reload_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _Archive_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _Restore_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _SetDefault_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _ClearDefault_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _Preview_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
    private:
        agents::AgentEditorViewModel _model;
        std::shared_ptr<agents::IAgentCatalogSource> _source;
        std::vector<agents::protocol::AgentProfileProjection> _profiles;
        std::vector<std::string> _visibleIds;
        std::optional<agents::protocol::WorkspaceAgentDefaultProjection> _default;
        std::string _workspace;
        std::string _after;
        std::function<void()> _closed;
        winrt::weak_ref<winrt::TerminalApp::IDialogPresenter> _presenter;
        Windows::UI::Xaml::DispatcherTimer _timer;
        uint64_t _generation{};
        uint64_t _editorGeneration{};
        bool _active{};
        bool _loadingEditor{};
        bool _listing{};
        bool _syncing{};
        void _refresh(bool more = false);
        void _renderList();
        void _load(std::string id);
        void _renderEditor();
        void _updateEnabled();
        void _execute(agents::protocol::AgentCatalogCommandRequest command);
        void _operation(agents::protocol::AgentCatalogOperation operation);
        safe_void_coroutine _archive();
        safe_void_coroutine _preview();
    };
}
namespace winrt::TerminalApp::factory_implementation { BASIC_FACTORY(AgentCatalogPage); }
