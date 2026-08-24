// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ConclusionSidebar.h"
#include "AgentHistoryItem.h"

#include "ConclusionSidebar.g.cpp"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
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

        TerminalApp::AgentHistoryItem _makeHistoryItem(const AgentHistoryRecord& record)
        {
            const auto item = winrt::make<implementation::AgentHistoryItem>();
            item.WorkspaceId(record.WorkspaceId);
            item.WorkspaceTitle(record.WorkspaceTitle);
            item.Provider(record.Provider);
            item.SessionId(record.SessionId);
            item.TurnId(record.TurnId);
            item.Cwd(record.Cwd);
            item.Title(record.Title);
            item.Prompt(record.Prompt);
            item.Message(record.FinalMarkdown);
            item.Model(record.Model);
            item.Timestamp(winrt::clock::from_sys(std::chrono::system_clock::time_point{ std::chrono::milliseconds{ record.EventAt } }));
            item.IsPinned(record.Pinned);
            return item;
        }
    }

    ConclusionSidebar::ConclusionSidebar() :
        _historyStore{ AgentHistoryStore::DefaultPath() },
        _historyItems{ winrt::single_threaded_observable_vector<TerminalApp::AgentHistoryItem>() }
    {
        InitializeComponent();
        _isInitialized = true;
        _ApplyLayout();
        _RefreshHistory();
        _UpdateContentState();
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::AgentHistoryItem> ConclusionSidebar::HistoryItems() const noexcept
    {
        return _historyItems;
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
            _RefreshHistory();
        }
    }

    bool ConclusionSidebar::AddPrompt(const winrt::guid& workspaceId,
                                      const winrt::hstring& workspaceTitle,
                                      const winrt::hstring& provider,
                                      const winrt::hstring& sessionId,
                                      const winrt::hstring& turnId,
                                      const winrt::hstring& cwd,
                                      const winrt::hstring& prompt,
                                      const winrt::hstring& model,
                                      const Windows::Foundation::DateTime& timestamp,
                                      const std::optional<AgentHistoryEventReceipt> event)
    {
        const auto eventAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 winrt::clock::to_sys(timestamp).time_since_epoch())
                                 .count();
        const auto receivedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();

        AgentHistoryRecord record;
        record.WorkspaceId = workspaceId;
        record.WorkspaceTitle = workspaceTitle;
        record.Provider = provider;
        record.SessionId = sessionId;
        record.TurnId = turnId;
        record.Cwd = cwd;
        record.Prompt = prompt;
        record.Model = model;
        record.EventAt = eventAt;
        record.ReceivedAt = receivedAt;
        if (!_historyStore.UpsertPrompt(record, event))
        {
            return false;
        }

        _RefreshHistory();
        return true;
    }

    bool ConclusionSidebar::AddConclusion(const winrt::guid& workspaceId,
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
                                          const std::optional<AgentHistoryEventReceipt> event)
    {
        const auto eventAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 winrt::clock::to_sys(timestamp).time_since_epoch())
                                 .count();
        const auto receivedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();

        AgentHistoryRecord record;
        record.WorkspaceId = workspaceId;
        record.WorkspaceTitle = workspaceTitle;
        record.Provider = provider;
        record.SessionId = sessionId;
        record.TurnId = turnId;
        record.Cwd = cwd;
        record.Title = title;
        record.Prompt = prompt;
        record.FinalMarkdown = finalMarkdown;
        record.Model = model;
        record.EventAt = eventAt;
        record.ReceivedAt = receivedAt;

        if (!_historyStore.UpsertFinal(record, event))
        {
            return false;
        }

        _RefreshHistory();
        return true;
    }

    void ConclusionSidebar::RefreshHistory()
    {
        _RefreshHistory();
    }

    void ConclusionSidebar::_Toggle_Click(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        if (!_isWindowNarrow)
        {
            IsExpanded(!_isExpanded);
        }
    }

    void ConclusionSidebar::_Scope_Click(const Windows::Foundation::IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto element = sender.try_as<FrameworkElement>())
        {
            _showAllWorkspaces = element.Name() == L"AllWorkspacesRadio";
            _RefreshHistory();
        }
    }

    void ConclusionSidebar::_Search_TextChanged(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        if (_isInitialized)
        {
            _RefreshHistory();
        }
    }

    void ConclusionSidebar::_Pin_Click(const Windows::Foundation::IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            if (_historyStore.SetTurnPinned(item.Provider(), item.SessionId(), item.TurnId(), !item.IsPinned()))
            {
                _RefreshHistory();
            }
        }
    }

    void ConclusionSidebar::_Copy_Click(const Windows::Foundation::IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            try
            {
                const auto text = item.HasPrompt() ?
                                      winrt::hstring{ RS_fmt(L"ConclusionSidebar_CopyPromptFormat", item.Prompt(), item.Message()) } :
                                      item.Message();
                DataPackage package;
                package.SetText(text);
                Clipboard::SetContent(package);
            }
            CATCH_LOG();
        }
    }

    void ConclusionSidebar::_Delete_Click(const Windows::Foundation::IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            if (_historyStore.DeleteTurn(item.Provider(), item.SessionId(), item.TurnId()))
            {
                _RefreshHistory();
            }
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

    void ConclusionSidebar::_RefreshHistory()
    {
        if (!_isInitialized)
        {
            return;
        }

        _historyItems.Clear();
        if (!_historyStore.IsAvailable() || (!_showAllWorkspaces && _activeWorkspaceId == winrt::guid{}))
        {
            _UpdateContentState();
            return;
        }

        std::optional<winrt::guid> workspaceId;
        if (!_showAllWorkspaces)
        {
            workspaceId = _activeWorkspaceId;
        }

        const auto search = HistorySearchBox().Text();
        const auto records = _historyStore.ListTurns(workspaceId,
                                                     std::wstring_view{ search.c_str(), search.size() },
                                                     MaximumHistory);
        for (const auto& record : records)
        {
            _historyItems.Append(_makeHistoryItem(record));
        }
        _UpdateContentState();
    }

    void ConclusionSidebar::_UpdateContentState()
    {
        if (!_isInitialized)
        {
            return;
        }

        const auto count = _historyItems.Size();
        const auto hasHistory = count > 0;
        HistoryList().Visibility(hasHistory ? Visibility::Visible : Visibility::Collapsed);
        EmptyState().Visibility(hasHistory ? Visibility::Collapsed : Visibility::Visible);
        CountBadge().Visibility(hasHistory ? Visibility::Visible : Visibility::Collapsed);
        CountText().Text(hasHistory ? winrt::to_hstring(count) : winrt::hstring{});
        WorkspaceNameText().Text(_showAllWorkspaces ?
                                     RS_(L"ConclusionSidebar_AllWorkspaces") :
                                     (_activeWorkspaceTitle.empty() ? RS_(L"ConclusionSidebar_WorkspaceFallback") : _activeWorkspaceTitle));
    }

    TerminalApp::AgentHistoryItem ConclusionSidebar::_ItemFromSender(const Windows::Foundation::IInspectable& sender) const
    {
        if (const auto element = sender.try_as<FrameworkElement>())
        {
            return element.Tag().try_as<TerminalApp::AgentHistoryItem>();
        }
        return nullptr;
    }
}
