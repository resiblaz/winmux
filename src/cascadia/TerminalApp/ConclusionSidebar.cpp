// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ConclusionSidebar.h"

#include "ConclusionSidebar.g.cpp"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Automation;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr std::wstring_view ConclusionsToolId{ L"conclusions" };

        std::wstring _toolKey(const winrt::hstring& id)
        {
            return { id.c_str(), id.size() };
        }
    }

    ConclusionSidebar::ConclusionSidebar() :
        _conclusions{ winrt::single_threaded_observable_vector<TerminalApp::WorkspaceNotification>() }
    {
        InitializeComponent();
        _ApplyLayout();
        _UpdateContentState();
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> ConclusionSidebar::Conclusions() const noexcept
    {
        return _conclusions;
    }

    bool ConclusionSidebar::IsExpanded() const noexcept
    {
        return _isExpanded;
    }

    void ConclusionSidebar::IsExpanded(const bool value)
    {
        if (_isExpanded != value)
        {
            _isExpanded = value;
            _ApplyLayout();
        }
    }

    bool ConclusionSidebar::RegisterToolTab(const winrt::hstring& id,
                                            const winrt::hstring& title,
                                            const winrt::hstring& iconGlyph,
                                            const Windows::UI::Xaml::UIElement& content)
    {
        const auto key = _toolKey(id);
        if (key.empty() || key == ConclusionsToolId || title.empty() || !content || _toolTabs.contains(key))
        {
            return false;
        }

        const auto tab = PivotItem{};
        const auto header = StackPanel{};
        header.Orientation(Orientation::Horizontal);
        header.Spacing(6);

        if (!iconGlyph.empty())
        {
            const auto icon = FontIcon{};
            icon.Glyph(iconGlyph);
            icon.FontSize(12);
            icon.Opacity(0.72);
            header.Children().Append(icon);
        }

        const auto label = TextBlock{};
        label.Text(title);
        label.FontSize(12);
        header.Children().Append(label);

        tab.Header(header);
        tab.Content(content);
        AutomationProperties::SetName(tab, title);
        SidebarTabs().Items().Append(tab);
        _toolTabs.emplace(key, tab);
        return true;
    }

    bool ConclusionSidebar::RemoveToolTab(const winrt::hstring& id)
    {
        const auto key = _toolKey(id);
        const auto entry = _toolTabs.find(key);
        if (entry == _toolTabs.end())
        {
            return false;
        }

        uint32_t index{};
        if (SidebarTabs().Items().IndexOf(entry->second, index))
        {
            SidebarTabs().Items().RemoveAt(index);
        }
        _toolTabs.erase(entry);
        return true;
    }

    bool ConclusionSidebar::SelectToolTab(const winrt::hstring& id)
    {
        const auto key = _toolKey(id);
        if (key == ConclusionsToolId)
        {
            SidebarTabs().SelectedItem(ConclusionsTab());
            return true;
        }

        if (const auto entry = _toolTabs.find(key); entry != _toolTabs.end())
        {
            SidebarTabs().SelectedItem(entry->second);
            return true;
        }
        return false;
    }

    void ConclusionSidebar::SetWindowNarrow(const bool value)
    {
        if (_isWindowNarrow != value)
        {
            _isWindowNarrow = value;
            _ApplyLayout();
        }
    }

    void ConclusionSidebar::SetActiveWorkspace(const winrt::guid& workspaceId, const winrt::hstring& workspaceTitle)
    {
        if (_activeWorkspaceId != workspaceId || _activeWorkspaceTitle != workspaceTitle)
        {
            _activeWorkspaceId = workspaceId;
            _activeWorkspaceTitle = workspaceTitle;
            _RebuildVisibleConclusions();
        }
    }

    void ConclusionSidebar::AddConclusion(const winrt::guid& workspaceId,
                                          const winrt::hstring& workspaceTitle,
                                          const winrt::hstring& title,
                                          const winrt::hstring& message,
                                          const Windows::Foundation::DateTime& timestamp)
    {
        const auto conclusion = TerminalApp::WorkspaceNotification{
            workspaceId,
            0,
            workspaceTitle,
            title,
            message,
            timestamp,
        };

        _history.insert(_history.begin(), conclusion);
        if (_history.size() > MaximumHistory)
        {
            _history.erase(_history.begin() + MaximumHistory, _history.end());
        }

        if (workspaceId == _activeWorkspaceId)
        {
            _conclusions.InsertAt(0, conclusion);
            if (_conclusions.Size() > MaximumHistory)
            {
                _conclusions.RemoveAtEnd();
            }
            _UpdateContentState();
        }
    }

    void ConclusionSidebar::_Toggle_Click(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        if (!_isWindowNarrow)
        {
            IsExpanded(!_isExpanded);
        }
    }

    void ConclusionSidebar::_ApplyLayout()
    {
        const auto expanded = _isExpanded && !_isWindowNarrow;
        Width(expanded ? ExpandedWidth : CollapsedWidth);
        ExpandedContent().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
        CompactRail().Visibility(expanded ? Visibility::Collapsed : Visibility::Visible);

        const auto automationName = expanded ?
                                        RS_(L"ConclusionSidebar_CollapseAutomationName") :
                                        RS_(L"ConclusionSidebar_ExpandAutomationName");
        AutomationProperties::SetName(HeaderToggleButton(), automationName);
        AutomationProperties::SetName(RailToggleButton(), automationName);
        ToolTipService::SetToolTip(HeaderToggleButton(), box_value(automationName));
        ToolTipService::SetToolTip(RailToggleButton(), box_value(automationName));
        HeaderToggleButton().Content(box_value(expanded ? L"\uE76C" : L"\uE76B"));
        RailToggleButton().Content(box_value(L"\uE76B"));
        RailToggleButton().IsEnabled(!_isWindowNarrow);
    }

    void ConclusionSidebar::_RebuildVisibleConclusions()
    {
        std::vector<TerminalApp::WorkspaceNotification> visible;
        if (_activeWorkspaceId != winrt::guid{})
        {
            visible.reserve(_history.size());
            for (const auto& conclusion : _history)
            {
                if (conclusion.WorkspaceId() == _activeWorkspaceId)
                {
                    visible.emplace_back(conclusion);
                }
            }
        }

        _conclusions.ReplaceAll({ visible.data(), gsl::narrow_cast<uint32_t>(visible.size()) });
        _UpdateContentState();
    }

    void ConclusionSidebar::_UpdateContentState()
    {
        const auto count = _conclusions.Size();
        const auto hasConclusions = count > 0;
        ConclusionList().Visibility(hasConclusions ? Visibility::Visible : Visibility::Collapsed);
        EmptyState().Visibility(hasConclusions ? Visibility::Collapsed : Visibility::Visible);
        CountBadge().Visibility(hasConclusions ? Visibility::Visible : Visibility::Collapsed);
        CountText().Text(hasConclusions ? winrt::to_hstring(count) : winrt::hstring{});
        WorkspaceNameText().Text(_activeWorkspaceTitle.empty() ? RS_(L"ConclusionSidebar_WorkspaceFallback") : _activeWorkspaceTitle);
    }
}
