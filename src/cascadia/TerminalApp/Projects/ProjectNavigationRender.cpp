// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "ProjectNavigationControls.h"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation::projects
{
    void ProjectNavigationControls::_RenderSelector(const ProjectNavigationState& state)
    {
        std::vector<ComboBoxItem> desired;
        for (const auto& id : state.OpenProjects)
        {
            auto& row = _rows.try_emplace(id, nullptr).first->second;
            if (!row)
            {
                row = ComboBoxItem{}; row.Tag(winrt::box_value(winrt::hstring{ til::u8u16(id) }));
            }
            const auto found = state.Catalog.find(id);
            const auto label = found == state.Catalog.end() ? winrt::hstring{ til::u8u16(id).substr(0, 8) } : winrt::hstring{ til::u8u16(found->second.Name) };
            // ComboBox borrows selected UIElement content for its collapsed presenter.
            // Use text data so subsequent refreshes never depend on that visual's ownership.
            if (winrt::unbox_value_or<winrt::hstring>(row.Content(), {}) != label) row.Content(winrt::box_value(label));
            desired.push_back(row);
        }
        for (uint32_t i = 0; i < desired.size(); ++i)
        {
            if (i < _projects.Items().Size() && _projects.Items().GetAt(i) == desired[i]) continue;
            uint32_t old{}; if (_projects.Items().IndexOf(desired[i], old)) _projects.Items().RemoveAt(old);
            _projects.Items().InsertAt(i, desired[i]);
        }
        while (_projects.Items().Size() > desired.size()) _projects.Items().RemoveAtEnd();
        const auto selected = _rows.find(state.ActiveProjectId);
        _projects.SelectedItem(selected == _rows.end() ? nullptr : selected->second);
    }
    void ProjectNavigationControls::_RenderLibrary(const ProjectNavigationState& state)
    {
        _library.Items().Clear();
        for (const auto& [id, project] : state.Catalog)
        {
            ListViewItem row; row.Tag(winrt::box_value(winrt::hstring{ til::u8u16(id) }));
            Automation::AutomationProperties::SetName(row, winrt::hstring{ til::u8u16(project.Name + " · " + (project.RootPath.empty() ? id.substr(0, 8) : project.RootPath)) });
            StackPanel content; content.Spacing(6); TextBlock name; name.FontSize(18); name.TextWrapping(TextWrapping::Wrap);
            name.Text(winrt::hstring{ til::u8u16(project.Name) + (project.Archived ? L" · " + std::wstring{ RS_(L"Project_Archived") } : std::wstring{}) });
            TextBlock path; path.TextWrapping(TextWrapping::Wrap);
            path.Text(project.Binding ? winrt::hstring{ til::u8u16(project.RootPath) } : RS_(L"Project_Unbound"));
            content.Children().Append(name); content.Children().Append(path); row.Content(content); _library.Items().Append(row);
        }
    }
    void ProjectNavigationControls::Render(const ProjectNavigationState& state, bool library)
    {
        _CaptureDraft(); _rendering = true;
        const auto restore = wil::scope_exit([&] { _rendering = false; });
        _active = state.ActiveProjectId; _notice.Text(state.Notice); _retry.Visibility(state.OutcomeUnknown ? Visibility::Visible : Visibility::Collapsed);
        const auto active = state.Active(); library = library || !active;
        _RenderSelector(state);
        Toolbar.FindName(L"Tasks").as<Button>().IsEnabled(active != nullptr);
        Toolbar.FindName(L"Overview").as<Button>().IsEnabled(active != nullptr);
        _library.Visibility(library ? Visibility::Visible : Visibility::Collapsed);
        Content.FindName(L"OverviewScroll").as<ScrollViewer>().Visibility(library ? Visibility::Collapsed : Visibility::Visible);
        _heading.Text(library ? RS_(L"Project_Library") : winrt::hstring{ til::u8u16(active->Name) });
        if (library) { _RenderLibrary(state); return; }
        auto& draft = _drafts[_active];
        if (!draft.Version || !draft.Dirty || (draft.Name == til::u8u16(active->Name) && draft.Description == til::u8u16(active->Description)))
            draft = { til::u8u16(active->Name), til::u8u16(active->Description), active->Version, false };
        _name.Text(draft.Name); _description.Text(draft.Description);
        _root.Text(active->Binding ? winrt::hstring{ til::u8u16(active->RootPath) } : RS_(L"Project_Unbound"));
        uint64_t total{}; for (auto count : active->StatusCounts) total += count;
        _counts.Text(winrt::hstring{ RS_fmt(L"Project_Counts", total, active->ActiveRuns) });
        const auto editable = !active->Archived && !state.CommandPending && !state.OutcomeUnknown;
        _name.IsReadOnly(!editable); _description.IsReadOnly(!editable); _save.IsEnabled(editable && state.Ready()); _bind.IsEnabled(editable && state.Ready());
        _archive.Visibility(active->Archived ? Visibility::Collapsed : Visibility::Visible);
        _restore.Visibility(active->Archived ? Visibility::Visible : Visibility::Collapsed);
        _archive.IsEnabled(state.Ready() && !state.CommandPending && !state.OutcomeUnknown); _restore.IsEnabled(_archive.IsEnabled());
    }
}
