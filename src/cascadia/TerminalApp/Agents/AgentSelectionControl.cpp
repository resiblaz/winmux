// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentSelectionControl.h"
#include "AgentSelectionControl.g.cpp"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    using namespace agents::protocol;

    AgentSelectionControl::AgentSelectionControl()
    {
        _syncing = true;
        InitializeComponent();
        _syncing = true;
        AgentPicker().Items().Append(box_value(RS_(L"Agent_Custom")));
        AgentPicker().SelectedIndex(0);
        _syncing = false;
        _manual();
    }

    void AgentSelectionControl::Configure(std::shared_ptr<agents::IAgentCatalogSource> source, std::string workspaceId,
                                         std::optional<TaskProjectionExecutionRecord> initial)
    {
        ++_generation; ++_selectionGeneration;
        _source = std::move(source); _workspace = std::move(workspaceId); _initial = std::move(initial);
        _initialLoad = true;
        _profiles.clear(); _selected.reset(); _after.clear();
        _syncing = true;
        AgentPicker().Items().Clear(); AgentPicker().Items().Append(box_value(RS_(L"Agent_Custom")));
        AgentPicker().SelectedIndex(0);
        AgentModel().Text(_initial ? til::u8u16(_initial->Model) : L"");
        AgentInstructions().Text(_initial ? til::u8u16(_initial->Instructions) : L"");
        _syncing = false;
        _manual();
        RefreshAgentsButton().Visibility(_source ? Visibility::Visible : Visibility::Collapsed);
        if (_source) _load(false);
    }

    void AgentSelectionControl::_error(const std::wstring& value)
    {
        SelectionError().Text(value);
        SelectionError().Visibility(value.empty() ? Visibility::Collapsed : Visibility::Visible);
    }
    void AgentSelectionControl::_notify() { if (_changed) _changed(); }

    void AgentSelectionControl::_load(const bool more)
    {
        if (!_source) return;
        const auto generation = _generation;
        AgentCatalogQueryRequest query;
        if (!_workspace.empty()) query.WorkspaceId = _workspace;
        if (more) query.AfterId = _after;
        else { _ready = false; AgentPicker().IsEnabled(false); _notify(); }
        MoreAgentsButton().IsEnabled(false); RefreshAgentsButton().IsEnabled(false);
        _source->Query(query, [weak = get_weak(), generation, more](auto reply) {
            const auto self = weak.get();
            if (!self || generation != self->_generation) return;
            self->AgentPicker().IsEnabled(true); self->MoreAgentsButton().IsEnabled(true); self->RefreshAgentsButton().IsEnabled(true);
            if (!reply.Succeeded)
            {
                self->_error(til::u8u16(reply.Error));
                if (!more) { self->_syncing = true; self->AgentPicker().SelectedIndex(-1); self->_syncing = false; self->_ready = false; }
                self->_notify(); return;
            }
            self->_syncing = true;
            if (!more)
            {
                self->_profiles.clear(); self->AgentPicker().Items().Clear(); self->AgentPicker().Items().Append(box_value(RS_(L"Agent_Custom")));
            }
            for (auto& profile : reply.Value.Profiles)
            {
                self->AgentPicker().Items().Append(box_value(hstring{ til::u8u16(profile.Configuration.Name) + L" · v" + std::to_wstring(profile.Revision) + L" · " + til::u8u16(profile.Id.substr(0, 8)) }));
                self->_profiles.emplace_back(std::move(profile));
            }
            self->_after = reply.Value.NextAfterId;
            self->MoreAgentsButton().Visibility(self->_after.empty() ? Visibility::Collapsed : Visibility::Visible);
            self->_syncing = false;
            if (more) return;
            self->_error({});
            const auto desired = !self->_initialLoad ? (self->_selected ? self->_selected->Id : std::string{}) :
                                 self->_initial ? (self->_initial->Agent ? self->_initial->Agent->Id : std::string{}) :
                                 reply.Value.Default && reply.Value.Default->AgentId ? *reply.Value.Default->AgentId : std::string{};
            self->_initialLoad = false;
            if (desired.empty())
            {
                self->_syncing = true; self->AgentPicker().SelectedIndex(0); self->_syncing = false; self->_manual();
            }
            else self->_select(desired);
        });
    }

    void AgentSelectionControl::_select(std::string id)
    {
        if (!_source) return;
        _ready = false; _selected.reset(); _notify();
        const auto generation = _generation;
        const auto selection = ++_selectionGeneration;
        AgentCatalogQueryRequest query; query.AgentId = id;
        _source->Query(query, [weak = get_weak(), generation, selection](auto reply) {
            const auto self = weak.get();
            if (!self || generation != self->_generation || selection != self->_selectionGeneration) return;
            if (!reply.Succeeded || reply.Value.Profiles.size() != 1 || reply.Value.Profiles.front().Archived)
            {
                self->_syncing = true; self->AgentPicker().SelectedIndex(-1); self->_syncing = false;
                self->_error(reply.Succeeded ? std::wstring{ RS_(L"Agent_Unavailable") } : til::u8u16(reply.Error));
                self->_notify(); return;
            }
            auto profile = std::move(reply.Value.Profiles.front());
            self->_syncing = true;
            const auto found = std::ranges::find(self->_profiles, profile.Id, &AgentProfileProjection::Id);
            size_t index;
            if (found == self->_profiles.end())
            {
                index = self->_profiles.size(); self->_profiles.push_back(profile);
                self->AgentPicker().Items().Append(box_value(hstring{ til::u8u16(profile.Configuration.Name) + L" · v" + std::to_wstring(profile.Revision) }));
            }
            else
            {
                index = static_cast<size_t>(found - self->_profiles.begin());
                *found = profile;
            }
            self->AgentPicker().Items().SetAt(static_cast<uint32_t>(index + 1), box_value(hstring{ til::u8u16(profile.Configuration.Name) + L" · v" + std::to_wstring(profile.Revision) + L" · " + til::u8u16(profile.Id.substr(0, 8)) }));
            self->AgentPicker().SelectedIndex(static_cast<int32_t>(index + 1));
            self->AgentModel().Text(til::u8u16(profile.Configuration.Model));
            self->AgentInstructions().Text(til::u8u16(profile.Configuration.Instructions));
            self->AgentModel().IsReadOnly(true); self->AgentInstructions().IsReadOnly(true);
            self->SelectionNote().Text(RS_(L"Agent_PinnedNote"));
            self->_selected = std::move(profile); self->_ready = true; self->_syncing = false;
            self->_error({}); self->_notify();
        });
    }

    void AgentSelectionControl::_manual()
    {
        ++_selectionGeneration;
        _selected.reset(); _ready = true;
        AgentModel().IsReadOnly(false); AgentInstructions().IsReadOnly(false);
        SelectionNote().Text(RS_(L"Agent_CustomNote"));
        _error({}); _notify();
    }

    void AgentSelectionControl::_SelectionChanged(const IInspectable&, const SelectionChangedEventArgs&)
    {
        if (_syncing) return;
        const auto index = AgentPicker().SelectedIndex();
        if (index == 0)
        {
            _syncing = true;
            if (_selected)
            {
                AgentModel().Text(til::u8u16(_selected->Configuration.Model));
                AgentInstructions().Text(til::u8u16(_selected->Configuration.Instructions));
            }
            _syncing = false; _manual();
        }
        else if (index > 0 && static_cast<size_t>(index) <= _profiles.size()) _select(_profiles[index - 1].Id);
    }
    void AgentSelectionControl::_TextChanged(const IInspectable&, const TextChangedEventArgs&)
    {
        if (_syncing) return;
        if (_ready && !_selected)
        {
            TaskAgentConfiguration config;
            config.Model = til::u16u8(AgentModel().Text()); config.Instructions = til::u16u8(AgentInstructions().Text());
            std::string error;
            _error(ValidateTaskAgentConfiguration(config, error) ? std::wstring{} : til::u8u16(error));
        }
        _notify();
    }
    void AgentSelectionControl::_More_Click(const IInspectable&, const RoutedEventArgs&) { _load(true); }
    void AgentSelectionControl::_Refresh_Click(const IInspectable&, const RoutedEventArgs&) { _load(false); }

    std::optional<TaskAgentConfiguration> AgentSelectionControl::Selection()
    {
        if (!_ready || AgentPicker().SelectedIndex() < 0) return std::nullopt;
        TaskAgentConfiguration result;
        if (_selected) result.Agent = AgentProfileReference{ _selected->Id, _selected->Revision };
        else { result.Model = til::u16u8(AgentModel().Text()); result.Instructions = til::u16u8(AgentInstructions().Text()); }
        std::string error;
        if (!ValidateTaskAgentConfiguration(result, error)) return std::nullopt;
        return result;
    }
    std::wstring AgentSelectionControl::SelectedName() const { return _selected ? til::u8u16(_selected->Configuration.Name) : std::wstring{}; }
}
