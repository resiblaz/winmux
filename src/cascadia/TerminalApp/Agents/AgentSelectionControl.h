// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "AgentSelectionControl.g.h"
#include "IAgentCatalogSource.h"
#include "AgentSelectionState.h"

namespace winrt::TerminalApp::implementation
{
    struct AgentSelectionControl : AgentSelectionControlT<AgentSelectionControl>
    {
        AgentSelectionControl();
        void Configure(std::shared_ptr<agents::IAgentCatalogSource> source, std::string workspaceId,
                       std::optional<agents::protocol::TaskProjectionExecutionRecord> initial = std::nullopt);
        std::optional<agents::protocol::TaskAgentConfiguration> Selection();
        std::wstring SelectedName() const;
        agents::AgentSelectionState CaptureState();
        void RestoreState(std::shared_ptr<agents::IAgentCatalogSource> source, std::string workspaceId, const agents::AgentSelectionState& state);
        void SetChangedHandler(std::function<void()> handler) { _changed = std::move(handler); }
        void _SelectionChanged(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs&);
        void _TextChanged(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::Controls::TextChangedEventArgs&);
        void _More_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _Refresh_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
    private:
        std::shared_ptr<agents::IAgentCatalogSource> _source;
        std::string _workspace;
        std::optional<agents::protocol::TaskProjectionExecutionRecord> _initial;
        std::vector<agents::protocol::AgentProfileProjection> _profiles;
        std::optional<agents::protocol::AgentProfileProjection> _selected;
        std::function<void()> _changed;
        std::string _after;
        uint64_t _generation{};
        uint64_t _selectionGeneration{};
        bool _syncing{};
        bool _ready{};
        bool _initialLoad{ true };
        bool _explicitSelection{};
        void _load(bool more);
        void _select(std::string id);
        void _manual();
        void _error(const std::wstring& value);
        void _notify();
    };
}
namespace winrt::TerminalApp::factory_implementation { BASIC_FACTORY(AgentSelectionControl); }
