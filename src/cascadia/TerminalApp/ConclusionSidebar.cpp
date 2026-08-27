// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ConclusionSidebar.h"
#include "AgentHistoryItem.h"
#include "CodexAppServerClient.h"
#include "WorkspaceAgentResume.h"

#include "ConclusionSidebar.g.cpp"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Automation;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace std::chrono_literals;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr std::wstring_view ConclusionsToolId{ L"conclusions" };
        constexpr std::wstring_view SessionsToolId{ L"sessions" };

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

        TerminalApp::AgentHistoryItem _makeSessionItem(const AgentHistorySessionRecord& record,
                                                       const bool canLoad,
                                                       const bool canResume)
        {
            const auto item = winrt::make<implementation::AgentHistoryItem>();
            item.WorkspaceId(record.WorkspaceId);
            item.WorkspaceTitle(record.WorkspaceTitle);
            item.Provider(record.Provider);
            item.SessionId(record.SessionId);
            item.Cwd(record.Cwd);
            item.Title(record.DisplayName.empty() ? RS_(L"ConclusionSidebar_UntitledSession") : record.DisplayName);
            item.Message(record.Preview);
            item.Model(record.Model);
            item.SourceKind(record.SourceKind);
            item.Timestamp(winrt::clock::from_sys(std::chrono::system_clock::time_point{ std::chrono::milliseconds{ record.UpdatedAt } }));
            item.IsSessionSummary(true);
            item.CanLoad(canLoad);
            item.CanResume(canResume);
            item.HasLocalTurns(record.HasLocalTurns);
            return item;
        }
    }

    ConclusionSidebar::ConclusionSidebar() :
        _historyStore{ AgentHistoryStore::DefaultPath() },
        _historyItems{ winrt::single_threaded_observable_vector<TerminalApp::AgentHistoryItem>() },
        _sessionItems{ winrt::single_threaded_observable_vector<TerminalApp::AgentHistoryItem>() }
    {
        InitializeComponent();
        _historyAvailable = _historyStore.IsAvailable();
        const auto weakThis = get_weak();
        _historySearchTimer.Interval(200ms);
        _historySearchTimer.Tick([weakThis](const auto&, const auto&) {
            if (const auto self = weakThis.get())
            {
                self->_historySearchTimer.Stop();
                self->_RefreshHistory();
            }
        });
        _sessionSearchTimer.Interval(200ms);
        _sessionSearchTimer.Tick([weakThis](const auto&, const auto&) {
            if (const auto self = weakThis.get())
            {
                self->_sessionSearchTimer.Stop();
                self->_RefreshSessions();
            }
        });
        _isInitialized = true;
        _ApplyLayout();
        _RefreshHistory();
        _RefreshSessions();
        _UpdateContentState();
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::AgentHistoryItem> ConclusionSidebar::HistoryItems() const noexcept
    {
        return _historyItems;
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::AgentHistoryItem> ConclusionSidebar::SessionItems() const noexcept
    {
        return _sessionItems;
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
        if (key.empty() || key == ConclusionsToolId || key == SessionsToolId || title.empty() || !content || _toolTabs.contains(key))
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
        if (key == SessionsToolId)
        {
            SidebarTabs().SelectedItem(SessionsTab());
            return true;
        }

        if (const auto entry = _toolTabs.find(key); entry != _toolTabs.end())
        {
            SidebarTabs().SelectedItem(entry->second);
            return true;
        }
        return false;
    }

    void ConclusionSidebar::SetTabContentMode(const bool value)
    {
        if (_isTabContent != value)
        {
            _isTabContent = value;
            _ApplyLayout();
        }
    }

    void ConclusionSidebar::SetWindowNarrow(const bool value)
    {
        if (_isWindowNarrow != value)
        {
            _isWindowNarrow = value;
            _ApplyLayout();
        }
    }

    void ConclusionSidebar::SetActiveWorkspace(const winrt::guid& workspaceId,
                                               const winrt::hstring& workspaceTitle,
                                               const winrt::hstring& cwd)
    {
        if (_activeWorkspaceId != workspaceId || _activeWorkspaceTitle != workspaceTitle || _activeWorkspaceCwd != cwd)
        {
            _activeWorkspaceId = workspaceId;
            _activeWorkspaceTitle = workspaceTitle;
            _activeWorkspaceCwd = cwd;
            _RefreshHistory();
            _RefreshSessions();
        }
    }

    void ConclusionSidebar::SetCodexHistoryHandlers(CodexHistorySyncHandler syncHandler,
                                                    CodexHistorySessionHandler loadHandler,
                                                    CodexHistorySessionHandler resumeHandler)
    {
        _codexSyncHandler = std::move(syncHandler);
        _codexLoadHandler = std::move(loadHandler);
        _codexResumeHandler = std::move(resumeHandler);
        _RefreshSessions();
    }

    void ConclusionSidebar::SetCodexOperationState(const bool busy, const CodexHistoryUiState state)
    {
        const auto availabilityChanged = (_codexUiState == CodexHistoryUiState::Unavailable) !=
                                         (state == CodexHistoryUiState::Unavailable);
        _codexOperationBusy = busy;
        _codexUiState = state;
        if (availabilityChanged)
        {
            _RefreshSessions();
        }
        else
        {
            _UpdateContentState();
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
        _RefreshSessions();
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
        _RefreshSessions();
        return true;
    }

    void ConclusionSidebar::RefreshHistory()
    {
        _RefreshHistory();
    }

    void ConclusionSidebar::RefreshSessions()
    {
        _RefreshSessions();
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
            _QueueHistoryRefresh();
        }
    }

    void ConclusionSidebar::_SessionFilter_TextChanged(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        if (_isInitialized)
        {
            _QueueHistoryRefresh();
        }
    }

    void ConclusionSidebar::_DateFilter_SelectionChanged(const Windows::Foundation::IInspectable&, const SelectionChangedEventArgs&)
    {
        if (_isInitialized)
        {
            _QueueHistoryRefresh();
        }
    }

    void ConclusionSidebar::_SessionSearch_TextChanged(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        if (_isInitialized)
        {
            _QueueSessionRefresh();
        }
    }

    void ConclusionSidebar::_SessionSelection_Changed(const Windows::Foundation::IInspectable&, const SelectionChangedEventArgs&)
    {
        _UpdateSessionActions();
    }

    void ConclusionSidebar::_SyncSessions_Click(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        if (_codexOperationBusy || !_codexSyncHandler ||
            _activeWorkspaceId == winrt::guid{} || _activeWorkspaceCwd.empty())
        {
            return;
        }

        try
        {
            _codexSyncHandler(_activeWorkspaceId, _activeWorkspaceTitle, _activeWorkspaceCwd);
        }
        catch (...)
        {
            SetCodexOperationState(false, CodexHistoryUiState::Error);
        }
    }

    void ConclusionSidebar::_LoadSession_Click(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        const auto item = _SelectedSession();
        if (_codexOperationBusy || !_codexLoadHandler || !item || !item.CanLoad())
        {
            return;
        }

        try
        {
            _codexLoadHandler(item.WorkspaceId(), item.WorkspaceTitle(), item.Cwd(), item.SessionId());
        }
        catch (...)
        {
            SetCodexOperationState(false, CodexHistoryUiState::Error);
        }
    }

    void ConclusionSidebar::_ResumeSession_Click(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        const auto item = _SelectedSession();
        if (_codexOperationBusy || !_codexResumeHandler || !item || !item.CanResume())
        {
            return;
        }

        try
        {
            _codexResumeHandler(item.WorkspaceId(), item.WorkspaceTitle(), item.Cwd(), item.SessionId());
        }
        catch (...)
        {
            SetCodexOperationState(false, CodexHistoryUiState::Error);
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
                _RefreshSessions();
            }
        }
    }

    safe_void_coroutine ConclusionSidebar::_ClearHistory_Click(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        if (_historyClearInFlight || !_historyAvailable || (!_showAllWorkspaces && _activeWorkspaceId == winrt::guid{}))
        {
            co_return;
        }

        const auto weakThis = get_weak();
        const auto clearAll = _showAllWorkspaces;
        const auto workspaceId = _activeWorkspaceId;
        _historyClearInFlight = true;
        ClearHistoryButton().IsEnabled(false);

        ContentDialog dialog{};
        dialog.Title(box_value(RS_(L"ConclusionSidebar_ClearHistoryTitle")));
        dialog.Content(box_value(clearAll ?
                                     RS_(L"ConclusionSidebar_ClearAllHistoryBody") :
                                     RS_(L"ConclusionSidebar_ClearWorkspaceHistoryBody")));
        dialog.PrimaryButtonText(RS_(L"ConclusionSidebar_ClearHistoryConfirm"));
        dialog.CloseButtonText(RS_(L"ConclusionSidebar_ClearHistoryCancel"));
        dialog.DefaultButton(ContentDialogButton::Close);
        if (co_await dialog.ShowAsync() != ContentDialogResult::Primary)
        {
            if (const auto self = weakThis.get())
            {
                self->_historyClearInFlight = false;
                self->_UpdateContentState();
            }
            co_return;
        }

        const auto self = weakThis.get();
        if (!self)
        {
            co_return;
        }
        const auto dispatcher = self->Dispatcher();

        co_await winrt::resume_background();
        AgentHistoryStore store{ AgentHistoryStore::DefaultPath() };
        const auto cleared = store.IsAvailable() &&
                             (clearAll ? store.DeleteAllUnpinned() : store.DeleteWorkspace(workspaceId));
        co_await wil::resume_foreground(dispatcher);

        if (const auto current = weakThis.get())
        {
            current->_historyClearInFlight = false;
            if (cleared)
            {
                current->_RefreshHistory();
                current->_RefreshSessions();
            }
            else
            {
                current->_UpdateContentState();
            }
        }
    }

    void ConclusionSidebar::_ApplyLayout()
    {
        if (_isTabContent)
        {
            Width(std::numeric_limits<double>::quiet_NaN());
            HorizontalAlignment(Windows::UI::Xaml::HorizontalAlignment::Stretch);
            RootBorder().BorderThickness(Thickness{});
            ExpandedContent().Visibility(Visibility::Visible);
            CompactRail().Visibility(Visibility::Collapsed);
            HeaderToggleButton().Visibility(Visibility::Collapsed);
            return;
        }

        const auto expanded = _isExpanded && !_isWindowNarrow;
        Width(expanded ? ExpandedWidth : CollapsedWidth);
        HorizontalAlignment(Windows::UI::Xaml::HorizontalAlignment::Right);
        RootBorder().BorderThickness(Thickness{ 1, 0, 0, 0 });
        ExpandedContent().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
        CompactRail().Visibility(expanded ? Visibility::Collapsed : Visibility::Visible);
        HeaderToggleButton().Visibility(Visibility::Visible);

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

    void ConclusionSidebar::_QueueHistoryRefresh()
    {
        if (_isInitialized)
        {
            _historySearchTimer.Stop();
            _historySearchTimer.Start();
        }
    }

    void ConclusionSidebar::_QueueSessionRefresh()
    {
        if (_isInitialized)
        {
            _sessionSearchTimer.Stop();
            _sessionSearchTimer.Start();
        }
    }

    safe_void_coroutine ConclusionSidebar::_RefreshHistory()
    {
        if (!_isInitialized)
        {
            co_return;
        }

        const auto generation = ++_historyQueryGeneration;
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();

        std::optional<winrt::guid> workspaceId;
        if (!_showAllWorkspaces)
        {
            workspaceId = _activeWorkspaceId;
        }

        AgentHistoryQuery query;
        query.WorkspaceId = workspaceId;
        query.SessionId = SessionFilterBox().Text();
        query.Search = HistorySearchBox().Text();
        query.Limit = MaximumHistory;
        const auto dateFilter = DateFilterBox().SelectedIndex();
        if (dateFilter > 0)
        {
            constexpr std::array<int64_t, 4> filterDays{ 0, 1, 7, 30 };
            if (static_cast<size_t>(dateFilter) < filterDays.size())
            {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                query.ReceivedFrom = now - filterDays.at(dateFilter) * 24 * 60 * 60 * 1000;
                query.ReceivedTo = now;
            }
        }

        co_await winrt::resume_background();
        AgentHistoryStore store{ AgentHistoryStore::DefaultPath() };
        const auto available = store.IsAvailable();
        auto records = available ? store.ListTurns(query) : std::vector<AgentHistoryRecord>{};
        co_await wil::resume_foreground(dispatcher);

        const auto self = weakThis.get();
        if (!self || generation != self->_historyQueryGeneration)
        {
            co_return;
        }

        self->_historyAvailable = available;
        self->_historyItems.Clear();
        for (const auto& record : records)
        {
            self->_historyItems.Append(_makeHistoryItem(record));
        }
        self->_UpdateContentState();
    }

    safe_void_coroutine ConclusionSidebar::_RefreshSessions()
    {
        if (!_isInitialized)
        {
            co_return;
        }

        const auto generation = ++_sessionQueryGeneration;
        const auto workspaceId = _activeWorkspaceId;
        if (workspaceId == winrt::guid{})
        {
            _sessionItems.Clear();
            _UpdateContentState();
            co_return;
        }

        const auto search = SessionSearchBox().Text();
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        co_await winrt::resume_background();
        AgentHistoryStore store{ AgentHistoryStore::DefaultPath() };
        const auto available = store.IsAvailable();
        auto records = available ?
                           store.ListSessionSummaries(workspaceId,
                                                      std::wstring_view{ search.c_str(), search.size() },
                                                      MaximumSessions) :
                           std::vector<AgentHistorySessionRecord>{};
        co_await wil::resume_foreground(dispatcher);

        const auto self = weakThis.get();
        if (!self || generation != self->_sessionQueryGeneration || workspaceId != self->_activeWorkspaceId)
        {
            co_return;
        }

        self->_historyAvailable = available;
        self->_sessionItems.Clear();
        for (const auto& record : records)
        {
            const std::wstring_view sessionId{ record.SessionId };
            const auto validSessionId = WorkspaceAgentResume::IsValidCodexSessionId(sessionId);
            const auto validOpaqueSessionId = !sessionId.empty() &&
                                              sessionId.size() <= CodexAppServerClient::MaximumIdentifierCharacters;
            self->_sessionItems.Append(_makeSessionItem(record,
                                                        static_cast<bool>(self->_codexLoadHandler) && validOpaqueSessionId,
                                                        static_cast<bool>(self->_codexResumeHandler) && validSessionId));
        }
        self->_UpdateContentState();
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
        ClearHistoryButton().IsEnabled(!_historyClearInFlight && _historyAvailable &&
                                       (_showAllWorkspaces || _activeWorkspaceId != winrt::guid{}));
        WorkspaceNameText().Text(_showAllWorkspaces ?
                                     RS_(L"ConclusionSidebar_AllWorkspaces") :
                                     (_activeWorkspaceTitle.empty() ? RS_(L"ConclusionSidebar_WorkspaceFallback") : _activeWorkspaceTitle));

        const auto sessionCount = _sessionItems.Size();
        const auto hasSessions = sessionCount > 0;
        SessionList().Visibility(hasSessions ? Visibility::Visible : Visibility::Collapsed);
        EmptySessionsState().Visibility(hasSessions ? Visibility::Collapsed : Visibility::Visible);
        SessionCountBadge().Visibility(hasSessions ? Visibility::Visible : Visibility::Collapsed);
        SessionCountText().Text(hasSessions ? winrt::to_hstring(sessionCount) : winrt::hstring{});
        SessionWorkspaceTitleText().Text(_activeWorkspaceTitle.empty() ?
                                             RS_(L"ConclusionSidebar_WorkspaceFallback") :
                                             _activeWorkspaceTitle);
        ActiveWorkspaceCwdText().Text(_activeWorkspaceCwd.empty() ?
                                          RS_(L"ConclusionSidebar_NoActiveWorkspaceCwd") :
                                          _activeWorkspaceCwd);
        ToolTipService::SetToolTip(ActiveWorkspaceCwdText(), box_value(ActiveWorkspaceCwdText().Text()));

        SessionProgressRing().IsActive(_codexOperationBusy);
        SessionProgressRing().Visibility(_codexOperationBusy ? Visibility::Visible : Visibility::Collapsed);
        switch (_codexUiState)
        {
        case CodexHistoryUiState::Ready:
            SessionStatusText().Text(RS_(L"ConclusionSidebar_CodexReady"));
            break;
        case CodexHistoryUiState::Syncing:
            SessionStatusText().Text(RS_(L"ConclusionSidebar_CodexSyncing"));
            break;
        case CodexHistoryUiState::Loading:
            SessionStatusText().Text(RS_(L"ConclusionSidebar_CodexLoading"));
            break;
        case CodexHistoryUiState::Resuming:
            SessionStatusText().Text(RS_(L"ConclusionSidebar_CodexResuming"));
            break;
        case CodexHistoryUiState::Error:
            SessionStatusText().Text(RS_(L"ConclusionSidebar_CodexError"));
            break;
        case CodexHistoryUiState::Unavailable:
        default:
            SessionStatusText().Text(RS_(L"ConclusionSidebar_CodexUnavailable"));
            break;
        }
        _UpdateSessionActions();
    }

    void ConclusionSidebar::_UpdateSessionActions()
    {
        if (!_isInitialized)
        {
            return;
        }

        const auto selected = _SelectedSession();
        SyncSessionsButton().IsEnabled(!_codexOperationBusy && static_cast<bool>(_codexSyncHandler) &&
                                           _activeWorkspaceId != winrt::guid{} && !_activeWorkspaceCwd.empty());
        SessionList().IsEnabled(!_codexOperationBusy);
        LoadSessionButton().IsEnabled(!_codexOperationBusy && selected && selected.CanLoad());
        ResumeSessionButton().IsEnabled(!_codexOperationBusy && selected && selected.CanResume());
    }

    TerminalApp::AgentHistoryItem ConclusionSidebar::_SelectedSession()
    {
        return SessionList().SelectedItem().try_as<TerminalApp::AgentHistoryItem>();
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
