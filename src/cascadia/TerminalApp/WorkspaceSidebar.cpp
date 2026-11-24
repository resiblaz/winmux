// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceSidebar.h"
#include "WorkspaceItemViewModel.h"
#include "Tab.h"
#include "Pane.h"

#include <robuffer.h>
#include <shellapi.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include "WorkspaceSidebar.g.cpp"

using namespace winrt;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Animation;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr uint32_t MaximumNotificationHistory{ 1000 };

        bool _isTabRunning(const TerminalApp::Tab& tab)
        {
            if (!tab)
            {
                return false;
            }
            if (const auto status = tab.TabStatus())
            {
                if (status.IsProgressRingActive() || status.IsProgressRingIndeterminate())
                {
                    return true;
                }
            }
            return false;
        }

        bool _isTabVimActive(const winrt::com_ptr<Tab>& /*tabImpl*/)
        {
            return false;
        }

        uint32_t _rootProcessIdForPane(const std::shared_ptr<Pane>& pane) noexcept
        {
            if (!pane)
            {
                return 0;
            }
            try
            {
                if (const auto control = pane->GetTerminalControl())
                {
                    if (const auto connection = control.Connection())
                    {
                        if (const auto conpty = connection.try_as<Microsoft::Terminal::TerminalConnection::ConptyConnection>())
                        {
                            if (const auto handle = reinterpret_cast<HANDLE>(conpty.RootProcessHandle()))
                            {
                                return GetProcessId(handle);
                            }
                        }
                    }
                }
            }
            catch (...)
            {
            }
            return 0;
        }

        bool _containsIgnoreCase(const std::wstring_view haystack, const std::wstring_view needle)
        {
            if (needle.empty() || haystack.size() < needle.size())
            {
                return false;
            }
            auto lower = [](const std::wstring_view text) {
                std::wstring out{ text };
                for (auto& ch : out)
                {
                    if (ch >= L'A' && ch <= L'Z')
                    {
                        ch = static_cast<wchar_t>(ch - L'A' + L'a');
                    }
                }
                return out;
            };
            return lower(haystack).find(lower(needle)) != std::wstring::npos;
        }

        std::vector<SidebarPaneInput> _collectLeafPanes(const winrt::com_ptr<Tab>& tabImpl,
                                                        const std::optional<uint32_t> activePaneId,
                                                        const std::unordered_map<uint32_t, SidebarProcessState>& processStates)
        {
            std::vector<SidebarPaneInput> panes;
            if (!tabImpl || !tabImpl->GetRootPane())
            {
                return panes;
            }

            const auto focusedPane = tabImpl->GetActivePane();
            const auto focusedId = activePaneId.has_value() ? activePaneId : (focusedPane ? focusedPane->Id() : std::nullopt);

            tabImpl->GetRootPane()->WalkTree([&](const auto& pane) {
                if (!pane || !pane->Id().has_value())
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }

                SidebarPaneInput entry{};
                entry.PaneId = pane->Id().value();
                const auto title = content.Title();
                entry.Title = title.empty() ?
                                  std::wstring{ RS_fmt(L"WorkspaceSidebar_PaneFallbackTitle", entry.PaneId + 1) } :
                                  std::wstring{ title };
                entry.IsActive = focusedId.has_value() && focusedId.value() == entry.PaneId;
                entry.HasAttention = pane->HasAttention();
                const auto state = content.TaskbarState();
                entry.IsRunning = state == 1 || state == 3;
                bool hasLiveProcessState = false;
                bool allowTitleFallback = true;
                if (const auto rootPid = _rootProcessIdForPane(pane); rootPid != 0)
                {
                    if (const auto it = processStates.find(rootPid); it != processStates.end())
                    {
                        hasLiveProcessState = true;
                        const auto& procState = it->second;
                        if (procState.HasActiveChild)
                        {
                            entry.IsRunning = true;
                            entry.ActiveProcessName = procState.ActiveProcessName;
                        }
                        if (procState.Icon != SidebarIconKind::Terminal)
                        {
                            entry.Icon = procState.Icon;
                            entry.UseExplicitIcon = true;
                            allowTitleFallback = false;
                        }
                        else if (!procState.HasActiveChild && !procState.AllowsTitleFallbackWhenIdle)
                        {
                            // Native Windows shell confirmed idle at prompt: revert stale agent title icons to Terminal.
                            entry.Icon = SidebarIconKind::Terminal;
                            entry.UseExplicitIcon = true;
                            allowTitleFallback = false;
                        }
                    }
                }
                if (allowTitleFallback && (!hasLiveProcessState || entry.Icon == SidebarIconKind::Terminal))
                {
                    entry.Icon = WorkspaceSidebarTreeModel::DetectIconKind(entry.Title);
                }
                panes.emplace_back(std::move(entry));
            });

            return panes;
        }
    }

    WorkspaceSidebar::WorkspaceSidebar() :
        _notificationHistory{ winrt::single_threaded_observable_vector<TerminalApp::WorkspaceNotification>() },
        _treeRows{ winrt::single_threaded_observable_vector<TerminalApp::WorkspaceSidebarRow>() }
    {
        InitializeComponent();

        const auto state = ApplicationState::SharedInstance();
        const auto savedWidth = state.WorkspaceSidebarWidth();
        const auto usesPreviousDefault = savedWidth == PreviousDefaultExpandedWidth ||
                                         savedWidth == LegacyDefaultExpandedWidth ||
                                         savedWidth == CompactLegacyDefaultExpandedWidth;
        _expandedWidth = NormalizeExpandedWidth(savedWidth);
        if (usesPreviousDefault)
        {
            state.WorkspaceSidebarWidth(_expandedWidth);
        }
        _isExpanded = state.WorkspaceSidebarExpanded();
        _ApplyLayout();

        Loaded([weakThis{ get_weak() }](auto&&, auto&&) {
            if (const auto self = weakThis.get())
            {
                self->_layoutElementsReady = true;
                self->_ApplyLayout();
                self->_layoutAnimationsEnabled = true;
                self->RefreshTree();
                const auto autoStart = WorkspaceRemoteBridge::LoadAutoStartPreference();
                self->RemoteBridgeAutoStartCheckBox().IsChecked(autoStart);
                if (autoStart)
                {
                    self->_AutoStartRemoteBridgeAsync();
                }
            }
        });
    }

    bool WorkspaceSidebar::IsExpanded() const noexcept
    {
        return _isExpanded;
    }

    void WorkspaceSidebar::IsExpanded(const bool value)
    {
        if (_isExpanded == value)
        {
            return;
        }

        _isExpanded = value;
        _ApplyLayout();
        ApplicationState::SharedInstance().WorkspaceSidebarExpanded(value);
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsExpanded" });
    }

    double WorkspaceSidebar::ExpandedWidth() const noexcept
    {
        return _expandedWidth;
    }

    void WorkspaceSidebar::ExpandedWidth(const double value)
    {
        const auto clamped = std::clamp(value, MinimumExpandedWidth, MaximumExpandedWidth);
        if (_expandedWidth == clamped)
        {
            return;
        }

        _expandedWidth = clamped;
        if (IsEffectivelyExpanded())
        {
            Width(_expandedWidth);
        }
        ApplicationState::SharedInstance().WorkspaceSidebarWidth(clamped);
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"ExpandedWidth" });
    }

    void WorkspaceSidebar::SetWindowNarrow(const bool value)
    {
        if (_isWindowNarrow != value)
        {
            _isWindowNarrow = value;
            _ApplyLayout();
        }
    }

    bool WorkspaceSidebar::IsWindowNarrow() const noexcept
    {
        return _isWindowNarrow;
    }

    bool WorkspaceSidebar::IsEffectivelyExpanded() const noexcept
    {
        return _isExpanded && !_isWindowNarrow;
    }

    double WorkspaceSidebar::NormalizeExpandedWidth(const double value) noexcept
    {
        const auto usesPreviousDefault = value == PreviousDefaultExpandedWidth ||
                                         value == LegacyDefaultExpandedWidth ||
                                         value == CompactLegacyDefaultExpandedWidth;
        return std::clamp(usesPreviousDefault ? DefaultExpandedWidth : value,
                          MinimumExpandedWidth,
                          MaximumExpandedWidth);
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> WorkspaceSidebar::NotificationHistory() const noexcept
    {
        return _notificationHistory;
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceSidebarRow> WorkspaceSidebar::TreeRows() const noexcept
    {
        return _treeRows;
    }

    void WorkspaceSidebar::SetWorkspaces(const Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel>& workspaces)
    {
        _workspacesChangedRevoker.revoke();
        _subscriptions.clear();
        _workspaces = workspaces;

        if (_workspaces)
        {
            _workspacesChangedRevoker = _workspaces.VectorChanged(winrt::auto_revoke, [weakThis{ get_weak() }](auto&&, auto&&) {
                if (const auto self = weakThis.get())
                {
                    self->_RebindWorkspaceSubscriptions();
                    self->ScheduleRefreshTree();
                }
            });
            _RebindWorkspaceSubscriptions();
        }

        WorkspaceList().ItemsSource(_treeRows);
        RefreshTree();
    }

    void WorkspaceSidebar::SetActiveSelection(const TerminalApp::Tab& activeTab, const std::optional<uint32_t> activePaneId)
    {
        _activeTab = activeTab ? winrt::make_weak(activeTab) : winrt::weak_ref<TerminalApp::Tab>{ nullptr };
        _activePaneId = activePaneId;
        if (_workspaces)
        {
            for (const auto& ws : _workspaces)
            {
                if (ws && ws.IsSelected())
                {
                    _treeModel.SetSelectedWorkspace(ws.WorkspaceId());
                    break;
                }
            }
        }
        RefreshTree();
    }

    void WorkspaceSidebar::UpdateProcessStates(std::unordered_map<uint32_t, SidebarProcessState> processStates)
    {
        if (_processStates == processStates)
        {
            return;
        }
        _processStates = std::move(processStates);
        ScheduleRefreshTree();
    }

    void WorkspaceSidebar::SetSearchQueryForTests(winrt::hstring query)
    {
        _treeModel.SetSearchQuery(std::wstring{ query });
        if (_layoutElementsReady && SidebarSearchBox().Text() != query)
        {
            SidebarSearchBox().Text(query);
        }
        RefreshTree();
    }

    void WorkspaceSidebar::SetFilterModeForTests(const SidebarFilterMode mode)
    {
        _treeModel.SetFilterMode(mode);
        RefreshTree();
    }

    bool WorkspaceSidebar::ToggleWorkspaceCollapsed(const winrt::guid& workspaceId)
    {
        const auto collapsed = _treeModel.ToggleWorkspaceCollapsed(workspaceId);
        RefreshTree();
        return collapsed;
    }

    bool WorkspaceSidebar::IsWorkspaceCollapsed(const winrt::guid& workspaceId) const noexcept
    {
        return _treeModel.IsWorkspaceCollapsed(workspaceId);
    }

    void WorkspaceSidebar::ScheduleRefreshTree()
    {
        if (_refreshScheduled)
        {
            return;
        }
        _refreshScheduled = true;
        const auto weakThis = get_weak();
        if (const auto dispatcher = Dispatcher())
        {
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                if (const auto self = weakThis.get())
                {
                    self->_refreshScheduled = false;
                    self->RefreshTree();
                }
            });
        }
        else
        {
            _refreshScheduled = false;
            RefreshTree();
        }
    }

    void WorkspaceSidebar::_RebindWorkspaceSubscriptions()
    {
        for (auto& wsSub : _subscriptions)
        {
            for (auto& sfSub : wsSub.Surfaces)
            {
                if (const auto tab = sfSub.WeakTab.get())
                {
                    if (const auto tabImpl = winrt::get_self<Tab>(tab); tabImpl && sfSub.PaneTreeChangedToken)
                    {
                        tabImpl->PaneTreeChanged(sfSub.PaneTreeChangedToken);
                    }
                }
            }
        }
        _subscriptions.clear();

        if (!_workspaces)
        {
            return;
        }

        const auto weakThis = get_weak();
        _subscriptions.reserve(_workspaces.Size());
        for (const auto& ws : _workspaces)
        {
            if (!ws)
            {
                continue;
            }
            WorkspaceSubscriptions wsSub{};
            wsSub.WorkspaceChanged = ws.PropertyChanged(winrt::auto_revoke, [weakThis](auto&&, const auto& args) {
                if (const auto self = weakThis.get())
                {
                    if (args.PropertyName() != L"SwitchShortcutHintText")
                    {
                        self->ScheduleRefreshTree();
                    }
                }
            });
            wsSub.SurfacesChanged = ws.Surfaces().VectorChanged(winrt::auto_revoke, [weakThis](auto&&, auto&&) {
                if (const auto self = weakThis.get())
                {
                    self->_RebindWorkspaceSubscriptions();
                    self->ScheduleRefreshTree();
                }
            });

            for (const auto& surface : ws.Surfaces())
            {
                if (!surface)
                {
                    continue;
                }
                SurfaceSubscriptions sfSub{};
                sfSub.WeakTab = winrt::make_weak(surface);
                sfSub.TabChanged = surface.PropertyChanged(winrt::auto_revoke, [weakThis](auto&&, const auto& args) {
                    if (const auto self = weakThis.get())
                    {
                        const auto name = args.PropertyName();
                        if (name == L"Title" || name == L"ReadOnly" || name == L"WorkingDirectory")
                        {
                            self->ScheduleRefreshTree();
                        }
                    }
                });
                if (const auto status = surface.TabStatus())
                {
                    sfSub.StatusChanged = status.PropertyChanged(winrt::auto_revoke, [weakThis](auto&&, auto&&) {
                        if (const auto self = weakThis.get())
                        {
                            self->ScheduleRefreshTree();
                        }
                    });
                }
                if (const auto tabImpl = winrt::get_self<Tab>(surface))
                {
                    sfSub.PaneTreeChangedToken = tabImpl->PaneTreeChanged([weakThis](auto&&, auto&&) {
                        if (const auto self = weakThis.get())
                        {
                            self->ScheduleRefreshTree();
                        }
                    });
                }
                wsSub.Surfaces.emplace_back(std::move(sfSub));
            }

            _subscriptions.emplace_back(std::move(wsSub));
        }
    }

    void WorkspaceSidebar::RefreshTree()
    {
        if (!_workspaces)
        {
            _treeRows.Clear();
            return;
        }

        const auto activeTab = _activeTab.get();
        std::vector<winrt::guid> activeWorkspaceIds;
        std::vector<SidebarWorkspaceInput> inputs;
        activeWorkspaceIds.reserve(_workspaces.Size());
        inputs.reserve(_workspaces.Size());

        for (const auto& ws : _workspaces)
        {
            if (!ws)
            {
                continue;
            }
            const auto wsImpl = winrt::get_self<WorkspaceItemViewModel>(ws);
            activeWorkspaceIds.push_back(ws.WorkspaceId());

            if (ws.IsSelected())
            {
                _treeModel.SetSelectedWorkspace(ws.WorkspaceId());
            }

            SidebarWorkspaceInput wsInput{};
            wsInput.WorkspaceId = ws.WorkspaceId();
            wsInput.Title = std::wstring{ ws.Title() };
            wsInput.Directory = std::wstring{ ws.WorkingDirectory() };
            wsInput.GitBranch = std::wstring{ ws.GitBranch() };
            wsInput.ListeningPorts = std::wstring{ ws.ListeningPortsDisplay() };
            wsInput.UnreadCount = ws.UnreadCount();
            wsInput.IsPinned = ws.IsPinned();
            wsInput.IsSelected = ws.IsSelected();
            wsInput.IsRunning = !ws.ListeningPortsDisplay().empty() ||
                                (wsImpl && wsImpl->AutomationProgressState() != 0);
            wsInput.HasAttention = ws.UnreadCount() > 0;

            for (const auto& surface : ws.Surfaces())
            {
                if (!surface)
                {
                    continue;
                }
                const auto tabImpl = winrt::get_self<Tab>(surface)->get_strong();
                const auto isCurrentSurface = activeTab && surface == activeTab;

                SidebarSurfaceInput sfInput{};
                sfInput.SurfaceId = wsImpl ? wsImpl->SurfaceIdForTab(surface) : winrt::guid{};
                sfInput.Title = std::wstring{ surface.Title() };
                if (const auto status = surface.TabStatus())
                {
                    sfInput.UnreadCount = status.UnreadNotificationCount();
                    sfInput.Subtitle = std::wstring{ status.NotificationText() };
                    sfInput.HasAttention = sfInput.UnreadCount > 0 || status.BellIndicator();
                }
                sfInput.IsActive = isCurrentSurface;
                sfInput.IsRunning = _isTabRunning(surface);
                sfInput.IsVimActive = _isTabVimActive(tabImpl);
                sfInput.Panes = _collectLeafPanes(tabImpl, isCurrentSurface ? _activePaneId : std::nullopt, _processStates);

                bool allPanesUseExplicitIcon = !sfInput.Panes.empty();
                std::wstring surfaceActiveProcess;
                SidebarIconKind activePaneIcon = SidebarIconKind::Terminal;
                SidebarIconKind firstAgentIcon = SidebarIconKind::Terminal;

                for (const auto& pane : sfInput.Panes)
                {
                    if (!pane.UseExplicitIcon)
                    {
                        allPanesUseExplicitIcon = false;
                    }
                    if (pane.HasAttention)
                    {
                        sfInput.HasAttention = true;
                    }
                    if (pane.IsRunning)
                    {
                        sfInput.IsRunning = true;
                    }
                    if (pane.IsActive && pane.Icon != SidebarIconKind::Terminal)
                    {
                        activePaneIcon = pane.Icon;
                    }
                    if (firstAgentIcon == SidebarIconKind::Terminal && pane.Icon != SidebarIconKind::Terminal)
                    {
                        firstAgentIcon = pane.Icon;
                    }
                    if (pane.IsActive && !pane.ActiveProcessName.empty())
                    {
                        surfaceActiveProcess = pane.ActiveProcessName;
                    }
                    else if (surfaceActiveProcess.empty() && !pane.ActiveProcessName.empty())
                    {
                        surfaceActiveProcess = pane.ActiveProcessName;
                    }
                }

                sfInput.Icon = activePaneIcon != SidebarIconKind::Terminal ? activePaneIcon : firstAgentIcon;
                if (sfInput.Icon != SidebarIconKind::Terminal || allPanesUseExplicitIcon)
                {
                    sfInput.UseExplicitIcon = true;
                }
                else
                {
                    sfInput.Icon = WorkspaceSidebarTreeModel::DetectIconKind(sfInput.Title);
                }
                if (sfInput.Subtitle.empty() &&
                    !surfaceActiveProcess.empty() &&
                    !_containsIgnoreCase(sfInput.Title, surfaceActiveProcess))
                {
                    sfInput.Subtitle = std::move(surfaceActiveProcess);
                }
                wsInput.Surfaces.emplace_back(std::move(sfInput));
            }
            inputs.emplace_back(std::move(wsInput));
        }

        _treeModel.PruneCollapsed(activeWorkspaceIds);
        const auto projection = _treeModel.Project(inputs);

        std::vector<winrt::com_ptr<WorkspaceSidebarRow>> nextRows;
        nextRows.reserve(projection.Nodes.size());

        for (const auto& node : projection.Nodes)
        {
            const auto ws = _workspaces.GetAt(gsl::narrow_cast<uint32_t>(node.WorkspaceInputIndex));
            const auto& wsInput = inputs[node.WorkspaceInputIndex];
            auto rowImpl = winrt::make_self<WorkspaceSidebarRow>();
            rowImpl->_workspace = ws;
            rowImpl->_hasAttention = node.Status == SidebarStatusKind::Attention;
            rowImpl->_isRunning = node.Status == SidebarStatusKind::Running;
            rowImpl->_isTerminalIcon = node.Icon == SidebarIconKind::Terminal;
            rowImpl->_isClaudeIcon = node.Icon == SidebarIconKind::Claude;
            rowImpl->_isCodexIcon = node.Icon == SidebarIconKind::Codex;
            rowImpl->_isAgyIcon = node.Icon == SidebarIconKind::Agy;

            if (node.Kind == SidebarNodeKind::Workspace)
            {
                rowImpl->_kind = TerminalApp::WorkspaceSidebarRowKind::Workspace;
                rowImpl->_title = ws.Title();
                rowImpl->_directoryDisplay = ws.WorkingDirectoryDisplay();
                rowImpl->_workingDirectory = ws.WorkingDirectory();
                rowImpl->_gitBranch = ws.GitBranch();
                rowImpl->_gitRepositoryRoot = ws.GitRepositoryRoot();
                rowImpl->_listeningPortsDisplay = ws.ListeningPortsDisplay();
                rowImpl->_listeningPortsTooltip = ws.ListeningPortsTooltip();
                rowImpl->_statusText = ws.StatusText();
                rowImpl->_unreadBadgeText = ws.UnreadBadgeText();
                rowImpl->_latestNotification = ws.LatestNotification();
                rowImpl->_surfaceCountText = winrt::to_hstring(ws.SurfaceCount());
                rowImpl->_switchShortcutHintText = _showWorkspaceSwitchHints ? ws.SwitchShortcutHintText() : winrt::hstring{};
                rowImpl->_chevronGlyph = node.IsCollapsed ? L"\uE76C" : L"\uE70D";
                rowImpl->_pinIconGlyph = ws.PinIconGlyph();
                rowImpl->_pinActionText = ws.PinActionText();
                rowImpl->_pinAutomationName = ws.PinAutomationName();
                rowImpl->_notificationAutomationName = ws.NotificationAutomationName();
                rowImpl->_notificationHistoryCountText = ws.NotificationHistoryCountText();
                rowImpl->_automationName = ws.AutomationName();
                rowImpl->_isPinned = ws.IsPinned();
                rowImpl->_isSelected = ws.IsSelected();
                rowImpl->_isActive = ws.IsSelected();
                rowImpl->_isCollapsed = node.IsCollapsed;
                rowImpl->_showCollapsedUnread = node.ShowCollapsedUnread && !ws.UnreadBadgeText().empty();
                rowImpl->_hasAgentResume = ws.HasAgentResume();
            }
            else if (node.Kind == SidebarNodeKind::Surface)
            {
                const auto surface = ws.Surfaces().GetAt(gsl::narrow_cast<uint32_t>(node.SurfaceInputIndex));
                const auto& sfInput = wsInput.Surfaces[node.SurfaceInputIndex];

                rowImpl->_kind = TerminalApp::WorkspaceSidebarRowKind::Surface;
                rowImpl->_surface = surface;
                rowImpl->_title = surface.Title();
                rowImpl->_subtitle = winrt::hstring{ sfInput.Subtitle };
                rowImpl->_unreadBadgeText = sfInput.UnreadCount == 0 ? winrt::hstring{} :
                                            sfInput.UnreadCount > 99 ? winrt::hstring{ L"99+" } :
                                                                       winrt::to_hstring(sfInput.UnreadCount);
                rowImpl->_paneCountBadgeText = sfInput.Panes.size() > 1 ?
                                                   winrt::hstring{ RS_fmt(L"WorkspaceSidebar_PaneCountBadge", sfInput.Panes.size()) } :
                                                   winrt::hstring{};
                rowImpl->_switchShortcutHintText = (_showSurfaceSwitchHints && ws.IsSelected()) ?
                                                       winrt::to_hstring(node.SurfaceOrdinal) :
                                                       winrt::hstring{};
                rowImpl->_isActive = sfInput.IsActive;
                rowImpl->_isSelected = sfInput.IsActive;
                rowImpl->_isVimActive = sfInput.IsVimActive;
                rowImpl->_automationName = surface.Title();
            }
            else
            {
                const auto surface = ws.Surfaces().GetAt(gsl::narrow_cast<uint32_t>(node.SurfaceInputIndex));
                const auto& sfInput = wsInput.Surfaces[node.SurfaceInputIndex];
                const auto& paneInput = sfInput.Panes[node.PaneInputIndex];

                rowImpl->_kind = TerminalApp::WorkspaceSidebarRowKind::Pane;
                rowImpl->_surface = surface;
                rowImpl->_paneId = paneInput.PaneId;
                rowImpl->_title = winrt::hstring{ paneInput.Title };
                rowImpl->_treeBranchGlyph = node.IsLastChild ? L"\u2514" : L"\u251C";
                rowImpl->_isActive = sfInput.IsActive && paneInput.IsActive;
                rowImpl->_isSelected = rowImpl->_isActive;
                rowImpl->_automationName = rowImpl->_title;
            }

            nextRows.emplace_back(std::move(rowImpl));
        }

        const auto wasSyncing = std::exchange(_syncingSelection, true);
        auto restoreSyncing = wil::scope_exit([&]() noexcept { _syncingSelection = wasSyncing; });

        // Incremental reconciliation: preserve existing WorkspaceSidebarRow instances and
        // ListViewItem containers across refreshes so scroll position and hover states never flicker.
        const auto existingSize = _treeRows.Size();
        const auto nextSize = gsl::narrow_cast<uint32_t>(nextRows.size());
        bool sameShape = existingSize == nextSize;
        if (sameShape)
        {
            for (uint32_t i = 0; i < nextSize; ++i)
            {
                const auto existingImpl = winrt::get_self<WorkspaceSidebarRow>(_treeRows.GetAt(i));
                if (!existingImpl || !existingImpl->HasSameIdentityAs(*nextRows[i]))
                {
                    sameShape = false;
                    break;
                }
            }
        }

        if (sameShape)
        {
            for (uint32_t i = 0; i < nextSize; ++i)
            {
                winrt::get_self<WorkspaceSidebarRow>(_treeRows.GetAt(i))->UpdateFrom(*nextRows[i]);
            }
        }
        else if (nextRows.empty())
        {
            if (_treeRows.Size() > 0)
            {
                _treeRows.Clear();
            }
        }
        else
        {
            // Pass 1: remove rows that no longer exist in the new projection.
            for (uint32_t i = _treeRows.Size(); i > 0; --i)
            {
                const auto existingImpl = winrt::get_self<WorkspaceSidebarRow>(_treeRows.GetAt(i - 1));
                bool stillPresent = false;
                if (existingImpl)
                {
                    for (const auto& nextRow : nextRows)
                    {
                        if (existingImpl->HasSameIdentityAs(*nextRow))
                        {
                            stillPresent = true;
                            break;
                        }
                    }
                }
                if (!stillPresent)
                {
                    _treeRows.RemoveAt(i - 1);
                }
            }

            // Pass 2: align surviving rows in-place and insert newly visible rows.
            for (uint32_t targetIdx = 0; targetIdx < nextSize; ++targetIdx)
            {
                const auto& desired = *nextRows[targetIdx];
                if (targetIdx < _treeRows.Size())
                {
                    const auto currentImpl = winrt::get_self<WorkspaceSidebarRow>(_treeRows.GetAt(targetIdx));
                    if (currentImpl && currentImpl->HasSameIdentityAs(desired))
                    {
                        currentImpl->UpdateFrom(desired);
                        continue;
                    }
                }

                std::optional<uint32_t> foundIdx;
                for (uint32_t searchIdx = targetIdx + 1; searchIdx < _treeRows.Size(); ++searchIdx)
                {
                    const auto candidateImpl = winrt::get_self<WorkspaceSidebarRow>(_treeRows.GetAt(searchIdx));
                    if (candidateImpl && candidateImpl->HasSameIdentityAs(desired))
                    {
                        foundIdx = searchIdx;
                        break;
                    }
                }

                if (foundIdx.has_value())
                {
                    const auto existingRow = _treeRows.GetAt(*foundIdx);
                    winrt::get_self<WorkspaceSidebarRow>(existingRow)->UpdateFrom(desired);
                    _treeRows.RemoveAt(*foundIdx);
                    _treeRows.InsertAt(targetIdx, existingRow);
                }
                else
                {
                    _treeRows.InsertAt(targetIdx, *nextRows[targetIdx]);
                }
            }

            while (_treeRows.Size() > nextSize)
            {
                _treeRows.RemoveAtEnd();
            }
        }

        _UpdateFilterHeader(projection);
        _SyncListSelection();
    }

    void WorkspaceSidebar::_UpdateFilterHeader(const SidebarProjection& projection)
    {
        if (!_layoutElementsReady)
        {
            return;
        }

        const FilterHeaderState nextState{
            projection.TotalSurfaceCount,
            projection.AttentionSurfaceCount,
            projection.RunningSurfaceCount,
            _treeModel.FilterMode(),
            projection.AnyWorkspaceExpanded,
            projection.IsFiltered && projection.Nodes.empty(),
        };
        if (_lastFilterHeaderState.has_value() && *_lastFilterHeaderState == nextState)
        {
            return;
        }
        _lastFilterHeaderState = nextState;

        FilterAllButton().Content(box_value(winrt::hstring{ RS_fmt(L"WorkspaceSidebar_FilterAll", projection.TotalSurfaceCount) }));
        FilterAttentionButton().Content(box_value(winrt::hstring{ RS_fmt(L"WorkspaceSidebar_FilterAttention", projection.AttentionSurfaceCount) }));
        FilterRunningButton().Content(box_value(winrt::hstring{ RS_fmt(L"WorkspaceSidebar_FilterRunning", projection.RunningSurfaceCount) }));

        const auto activeBg = Application::Current().Resources().TryLookup(box_value(L"WorkspaceChromeButtonPressedBrush")).try_as<Brush>();
        const auto activeBorder = Application::Current().Resources().TryLookup(box_value(L"WorkspaceChromeDividerBrush")).try_as<Brush>();
        const auto activeFg = Application::Current().Resources().TryLookup(box_value(L"WorkspaceChromeTextPrimaryBrush")).try_as<Brush>();
        const auto inactiveFg = Application::Current().Resources().TryLookup(box_value(L"WorkspaceChromeTextSecondaryBrush")).try_as<Brush>();
        const auto transparent = SolidColorBrush{ Windows::UI::Colors::Transparent() };

        const auto applyPillState = [&](const Button& button, const bool active) {
            button.Background(active && activeBg ? activeBg : transparent);
            button.BorderBrush(active && activeBorder ? activeBorder : transparent);
            if (active && activeFg)
            {
                button.Foreground(activeFg);
            }
            else if (!active && inactiveFg)
            {
                button.Foreground(inactiveFg);
            }
        };

        const auto mode = _treeModel.FilterMode();
        applyPillState(FilterAllButton(), mode == SidebarFilterMode::All);
        applyPillState(FilterAttentionButton(), mode == SidebarFilterMode::Attention);
        applyPillState(FilterRunningButton(), mode == SidebarFilterMode::Running);

        CollapseAllButton().Content(box_value(projection.AnyWorkspaceExpanded ? L"\uE73F" : L"\uE740"));
        const auto collapseTooltip = projection.AnyWorkspaceExpanded ?
                                         RS_(L"WorkspaceSidebar_CollapseAllTooltip") :
                                         RS_(L"WorkspaceSidebar_ExpandAllTooltip");
        ToolTipService::SetToolTip(CollapseAllButton(), box_value(collapseTooltip));
        Automation::AutomationProperties::SetName(CollapseAllButton(), collapseTooltip);

        SidebarFilterEmptyState().Visibility(projection.IsFiltered && projection.Nodes.empty() ?
                                                 Visibility::Visible :
                                                 Visibility::Collapsed);
    }

    void WorkspaceSidebar::_SyncListSelection()
    {
        if (!_layoutElementsReady)
        {
            return;
        }

        // Only update selection if WorkspaceList is currently bound to _treeRows.
        if (WorkspaceList().ItemsSource().try_as<Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceSidebarRow>>() != _treeRows)
        {
            return;
        }

        int32_t targetIndex = -1;
        int32_t fallbackSurfaceIndex = -1;
        int32_t fallbackWorkspaceIndex = -1;
        for (uint32_t i = 0; i < _treeRows.Size(); ++i)
        {
            const auto row = _treeRows.GetAt(i);
            if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Pane && row.IsActive())
            {
                targetIndex = gsl::narrow_cast<int32_t>(i);
                break;
            }
            if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Surface && row.IsActive())
            {
                fallbackSurfaceIndex = gsl::narrow_cast<int32_t>(i);
            }
            if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Workspace && row.IsSelected())
            {
                fallbackWorkspaceIndex = gsl::narrow_cast<int32_t>(i);
            }
        }
        if (targetIndex < 0)
        {
            targetIndex = fallbackSurfaceIndex >= 0 ? fallbackSurfaceIndex : fallbackWorkspaceIndex;
        }

        const auto wasSyncing = std::exchange(_syncingSelection, true);
        auto restoreSyncing = wil::scope_exit([&]() noexcept { _syncingSelection = wasSyncing; });
        if (WorkspaceList().SelectedIndex() != targetIndex)
        {
            WorkspaceList().SelectedIndex(targetIndex);
        }
    }

    void WorkspaceSidebar::RefreshNotifications()
    {
        std::vector<TerminalApp::WorkspaceNotification> history;
        uint32_t unreadCount = 0;

        auto collectFromIterable = [&](const Windows::Foundation::Collections::IIterable<IInspectable>& workspaces) {
            for (const auto& value : workspaces)
            {
                if (const auto item = value.try_as<TerminalApp::WorkspaceItemViewModel>())
                {
                    unreadCount += item.UnreadCount();
                    for (const auto& notification : item.Notifications())
                    {
                        history.emplace_back(notification);
                    }
                }
            }
        };

        // Preserve compatibility with tests that bind WorkspaceItemViewModels directly
        // to WorkspaceList().ItemsSource(), while preferring _workspaces in normal operation.
        bool collectedFromListSource = false;
        if (const auto listSource = WorkspaceList().ItemsSource().try_as<Windows::Foundation::Collections::IIterable<IInspectable>>())
        {
            for (const auto& value : listSource)
            {
                if (value.try_as<TerminalApp::WorkspaceItemViewModel>())
                {
                    collectedFromListSource = true;
                }
                break;
            }
            if (collectedFromListSource)
            {
                collectFromIterable(listSource);
            }
        }

        if (!collectedFromListSource && _workspaces)
        {
            for (const auto& item : _workspaces)
            {
                if (item)
                {
                    unreadCount += item.UnreadCount();
                    for (const auto& notification : item.Notifications())
                    {
                        history.emplace_back(notification);
                    }
                }
            }
        }

        std::sort(history.begin(), history.end(), [](const auto& left, const auto& right) {
            return left.Timestamp() > right.Timestamp();
        });
        if (history.size() > MaximumNotificationHistory)
        {
            history.erase(history.begin() + MaximumNotificationHistory, history.end());
        }

        _notificationHistory.ReplaceAll({ history.data(), gsl::narrow_cast<uint32_t>(history.size()) });

        const auto hasHistory = !history.empty();
        NotificationCenterBadgeText().Text(unreadCount == 0 ? winrt::hstring{} : winrt::to_hstring(unreadCount));
        NotificationCenterBadge().Visibility(unreadCount == 0 ? Visibility::Collapsed : Visibility::Visible);
        NotificationHistoryList().Visibility(hasHistory ? Visibility::Visible : Visibility::Collapsed);
        NotificationCenterEmptyState().Visibility(hasHistory ? Visibility::Collapsed : Visibility::Visible);
        MarkAllNotificationsReadButton().IsEnabled(unreadCount > 0);
        ClearAllNotificationHistoryButton().IsEnabled(hasHistory);

        const auto automationName = unreadCount == 0 ?
                                        RS_(L"WorkspaceSidebar_NotificationCenterAutomationName") :
                                        winrt::hstring{ RS_fmt(L"WorkspaceSidebar_NotificationCenterUnreadAutomationName", unreadCount) };
        Automation::AutomationProperties::SetName(NotificationCenterButton(), automationName);

        if (_workspaces)
        {
            ScheduleRefreshTree();
        }
    }

    void WorkspaceSidebar::OpenNotificationCenter(const FrameworkElement& target)
    {
        RefreshNotifications();
        NotificationCenterFlyout().ShowAt(target);
    }

    void WorkspaceSidebar::ShowSwitchHints(const bool value, const bool surfaceHints)
    {
        if (surfaceHints)
        {
            _showSurfaceSwitchHints = value;
        }
        else
        {
            _showWorkspaceSwitchHints = value;
            if (!value)
            {
                _showSurfaceSwitchHints = false;
            }
        }

        if (_workspaces)
        {
            for (const auto& item : _workspaces)
            {
                if (item)
                {
                    winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item)->ShowSwitchShortcutHint(_showWorkspaceSwitchHints);
                }
            }
        }
        else if (const auto workspaces = WorkspaceList().ItemsSource().try_as<Windows::Foundation::Collections::IIterable<IInspectable>>())
        {
            for (const auto& workspace : workspaces)
            {
                if (const auto item = workspace.try_as<TerminalApp::WorkspaceItemViewModel>())
                {
                    winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item)->ShowSwitchShortcutHint(_showWorkspaceSwitchHints);
                }
            }
        }

        uint32_t activeSurfaceOrdinal = 0;
        for (const auto& row : _treeRows)
        {
            const auto rowImpl = winrt::get_self<WorkspaceSidebarRow>(row);
            if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Workspace)
            {
                activeSurfaceOrdinal = 0;
                rowImpl->SetSwitchShortcutHintText(_showWorkspaceSwitchHints && row.Workspace() ?
                                                       row.Workspace().SwitchShortcutHintText() :
                                                       winrt::hstring{});
            }
            else if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Surface)
            {
                ++activeSurfaceOrdinal;
                rowImpl->SetSwitchShortcutHintText(_showSurfaceSwitchHints && row.Workspace() && row.Workspace().IsSelected() ?
                                                       winrt::to_hstring(activeSurfaceOrdinal) :
                                                       winrt::hstring{});
            }
        }
    }

    void WorkspaceSidebar::_SetExpandedContentVisibility(const Windows::UI::Xaml::Visibility visibility)
    {
        SidebarHeader().Visibility(visibility);
        SidebarTitle().Visibility(visibility);
        WorkspaceList().Visibility(visibility);
    }

    void WorkspaceSidebar::_ApplyLayout()
    {
        const auto expanded = IsEffectivelyExpanded();
        const auto targetWidth = expanded ? _expandedWidth : CollapsedWidth;
        const auto targetOpacity = expanded ? 1.0 : 0.0;

        if (!_layoutElementsReady)
        {
            Width(targetWidth);
            return;
        }

        ResizeHandle().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
        _SetExpandedContentVisibility(expanded ? Visibility::Visible : Visibility::Collapsed);

        if (_layoutAnimationsEnabled)
        {
            const auto startWidth = ActualWidth();
            const auto startOpacity = SidebarChrome().Opacity();
            if (_layoutStoryboard)
            {
                _layoutStoryboard.Stop();
            }

            Width(targetWidth);
            SidebarChrome().Opacity(targetOpacity);

            const auto duration = DurationHelper::FromTimeSpan(Windows::Foundation::TimeSpan{ std::chrono::milliseconds{ 150 } });
            _layoutStoryboard = Storyboard{};
            _layoutStoryboard.Duration(duration);

            DoubleAnimation widthAnimation{};
            widthAnimation.Duration(duration);
            widthAnimation.From(startWidth);
            widthAnimation.To(targetWidth);
            widthAnimation.EasingFunction(QuadraticEase{});
            widthAnimation.EnableDependentAnimation(true);
            widthAnimation.FillBehavior(FillBehavior::Stop);
            _layoutStoryboard.Children().Append(widthAnimation);
            _layoutStoryboard.SetTarget(widthAnimation, *this);
            _layoutStoryboard.SetTargetProperty(widthAnimation, L"Width");

            DoubleAnimation opacityAnimation{};
            opacityAnimation.Duration(DurationHelper::FromTimeSpan(Windows::Foundation::TimeSpan{ std::chrono::milliseconds{ 90 } }));
            opacityAnimation.From(startOpacity);
            opacityAnimation.To(targetOpacity);
            opacityAnimation.FillBehavior(FillBehavior::Stop);
            _layoutStoryboard.Children().Append(opacityAnimation);
            _layoutStoryboard.SetTarget(opacityAnimation, SidebarChrome());
            _layoutStoryboard.SetTargetProperty(opacityAnimation, L"Opacity");

            _layoutStoryboard.Begin();
        }
        else
        {
            Width(targetWidth);
            SidebarChrome().Opacity(targetOpacity);
            _SetExpandedContentVisibility(expanded ? Visibility::Visible : Visibility::Collapsed);
        }

        ToggleButton().Content(box_value(expanded ? L"\uE76B" : L"\uE76C"));
        ToggleButton().IsEnabled(!_isWindowNarrow);
        Automation::AutomationProperties::SetName(ToggleButton(),
                                                  _isWindowNarrow ? RS_(L"WorkspaceSidebar_NarrowAutomationName") :
                                                  expanded        ? RS_(L"WorkspaceSidebar_CollapseAutomationName") :
                                                                    RS_(L"WorkspaceSidebar_ExpandAutomationName"));
        Automation::AutomationProperties::SetHelpText(ToggleButton(), _isWindowNarrow ? RS_(L"WorkspaceSidebar_NarrowHelpText") : winrt::hstring{});
    }

    TerminalApp::WorkspaceSidebarRow WorkspaceSidebar::_RowFromSender(const IInspectable& sender) const
    {
        if (const auto element = sender.try_as<FrameworkElement>())
        {
            return element.Tag().try_as<TerminalApp::WorkspaceSidebarRow>();
        }
        return nullptr;
    }

    TerminalApp::WorkspaceItemViewModel WorkspaceSidebar::_ItemFromSender(const IInspectable& sender) const
    {
        if (const auto row = _RowFromSender(sender))
        {
            return row.Workspace();
        }
        if (const auto element = sender.try_as<FrameworkElement>())
        {
            return element.Tag().try_as<TerminalApp::WorkspaceItemViewModel>();
        }
        return nullptr;
    }

    TerminalApp::Tab WorkspaceSidebar::_SurfaceFromSender(const IInspectable& sender) const
    {
        if (const auto row = _RowFromSender(sender))
        {
            return row.Surface();
        }
        if (const auto element = sender.try_as<FrameworkElement>())
        {
            return element.Tag().try_as<TerminalApp::Tab>();
        }
        return nullptr;
    }

    void WorkspaceSidebar::_Toggle_Click(const IInspectable&, const RoutedEventArgs&)
    {
        IsExpanded(!_isExpanded);
    }

    void WorkspaceSidebar::_CollapseAll_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (!_workspaces)
        {
            return;
        }
        std::vector<winrt::guid> ids;
        ids.reserve(_workspaces.Size());
        for (const auto& ws : _workspaces)
        {
            if (ws)
            {
                ids.push_back(ws.WorkspaceId());
            }
        }
        _treeModel.ToggleCollapseAll(ids);
        RefreshTree();
    }

    void WorkspaceSidebar::_CreateWorkspace_Click(const IInspectable&, const RoutedEventArgs&)
    {
        NewWorkspaceRequested.raise(*this, nullptr);
    }

    void WorkspaceSidebar::_SearchBox_TextChanged(const IInspectable&, const TextChangedEventArgs&)
    {
        const auto query = SidebarSearchBox().Text();
        SidebarSearchClearButton().Visibility(query.empty() ? Visibility::Collapsed : Visibility::Visible);
        _treeModel.SetSearchQuery(std::wstring{ query });
        RefreshTree();
    }

    void WorkspaceSidebar::_SearchClear_Click(const IInspectable&, const RoutedEventArgs&)
    {
        SidebarSearchBox().Text(L"");
    }

    void WorkspaceSidebar::_FilterAll_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _treeModel.SetFilterMode(SidebarFilterMode::All);
        RefreshTree();
    }

    void WorkspaceSidebar::_FilterAttention_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _treeModel.SetFilterMode(SidebarFilterMode::Attention);
        RefreshTree();
    }

    void WorkspaceSidebar::_FilterRunning_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _treeModel.SetFilterMode(SidebarFilterMode::Running);
        RefreshTree();
    }

    void WorkspaceSidebar::_Resize_DragDelta(const IInspectable&, const Controls::Primitives::DragDeltaEventArgs& args)
    {
        if (IsEffectivelyExpanded())
        {
            ExpandedWidth(_expandedWidth + args.HorizontalChange());
        }
    }

    void WorkspaceSidebar::_Resize_PreviewKeyDown(const IInspectable&, const Input::KeyRoutedEventArgs& args)
    {
        switch (args.OriginalKey())
        {
        case Windows::System::VirtualKey::Left:
            ExpandedWidth(_expandedWidth - 8.0);
            break;
        case Windows::System::VirtualKey::Right:
            ExpandedWidth(_expandedWidth + 8.0);
            break;
        case Windows::System::VirtualKey::Home:
            ExpandedWidth(MinimumExpandedWidth);
            break;
        case Windows::System::VirtualKey::End:
            ExpandedWidth(MaximumExpandedWidth);
            break;
        default:
            return;
        }
        args.Handled(true);
    }

    void WorkspaceSidebar::_SelectRelativeRow(const int32_t delta)
    {
        const auto count = gsl::narrow_cast<int32_t>(_treeRows.Size());
        if (count == 0)
        {
            return;
        }
        auto current = WorkspaceList().SelectedIndex();
        if (current < 0)
        {
            current = 0;
        }
        const auto next = (current + delta + count) % count;
        const auto row = _treeRows.GetAt(gsl::narrow_cast<uint32_t>(next));
        WorkspaceList().SelectedIndex(next);
        if (const auto container = WorkspaceList().ContainerFromIndex(next).try_as<FrameworkElement>())
        {
            container.StartBringIntoView();
        }
        ActivateRowRequested.raise(*this, row);
    }

    void WorkspaceSidebar::_WorkspaceList_KeyDown(const IInspectable&, const Input::KeyRoutedEventArgs& args)
    {
        const auto row = WorkspaceList().SelectedItem().try_as<TerminalApp::WorkspaceSidebarRow>();
        if (!row)
        {
            return;
        }

        switch (args.OriginalKey())
        {
        case Windows::System::VirtualKey::J:
            _SelectRelativeRow(1);
            break;
        case Windows::System::VirtualKey::K:
            _SelectRelativeRow(-1);
            break;
        case Windows::System::VirtualKey::Left:
            if (row.Workspace())
            {
                const auto wsId = row.Workspace().WorkspaceId();
                if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Workspace && !row.IsCollapsed())
                {
                    _treeModel.SetWorkspaceCollapsed(wsId, true);
                    RefreshTree();
                }
                else if (row.Kind() != TerminalApp::WorkspaceSidebarRowKind::Workspace)
                {
                    for (uint32_t i = 0; i < _treeRows.Size(); ++i)
                    {
                        const auto candidate = _treeRows.GetAt(i);
                        if (candidate.Kind() == TerminalApp::WorkspaceSidebarRowKind::Workspace &&
                            candidate.Workspace() &&
                            candidate.Workspace().WorkspaceId() == wsId)
                        {
                            WorkspaceList().SelectedIndex(gsl::narrow_cast<int32_t>(i));
                            break;
                        }
                    }
                }
            }
            break;
        case Windows::System::VirtualKey::Right:
            if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Workspace && row.Workspace())
            {
                const auto wsId = row.Workspace().WorkspaceId();
                if (row.IsCollapsed())
                {
                    _treeModel.SetWorkspaceCollapsed(wsId, false);
                    RefreshTree();
                }
                else
                {
                    _SelectRelativeRow(1);
                }
            }
            break;
        case Windows::System::VirtualKey::Enter:
            ActivateRowRequested.raise(*this, row);
            break;
        case Windows::System::VirtualKey::F2:
            if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Surface && row.Surface())
            {
                RenameSurfaceRequested.raise(*this, row.Surface());
            }
            else if (row.Workspace())
            {
                RenameRequested.raise(*this, row.Workspace());
            }
            break;
        case Windows::System::VirtualKey::Delete:
            if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Surface && row.Surface())
            {
                CloseSurfaceRequested.raise(*this, row.Surface());
            }
            else if (row.Workspace())
            {
                CloseRequested.raise(*this, row.Workspace());
            }
            break;
        default:
            return;
        }
        args.Handled(true);
    }

    void WorkspaceSidebar::_WorkspaceList_ItemClick(const IInspectable&, const ItemClickEventArgs& args)
    {
        if (const auto row = args.ClickedItem().try_as<TerminalApp::WorkspaceSidebarRow>())
        {
            if (row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Workspace &&
                row.Workspace() &&
                row.Workspace().IsSelected())
            {
                ToggleWorkspaceCollapsed(row.Workspace().WorkspaceId());
                return;
            }
            ActivateRowRequested.raise(*this, row);
        }
    }

    void WorkspaceSidebar::_WorkspaceList_DragItemsStarting(const IInspectable&, const DragItemsStartingEventArgs& args)
    {
        _dragRow = nullptr;
        if (args.Items().Size() != 1)
        {
            args.Cancel(true);
            return;
        }

        const auto row = args.Items().GetAt(0).try_as<TerminalApp::WorkspaceSidebarRow>();
        if (!row || row.Kind() == TerminalApp::WorkspaceSidebarRowKind::Pane)
        {
            args.Cancel(true);
            return;
        }

        _dragRow = row;
        args.Data().RequestedOperation(DataPackageOperation::Move);
    }

    void WorkspaceSidebar::_WorkspaceList_DragItemsCompleted(const IInspectable&, const DragItemsCompletedEventArgs&)
    {
        _dragRow = nullptr;
    }

    void WorkspaceSidebar::_TreeRow_DragOver(const IInspectable& sender, const DragEventArgs& args)
    {
        if (!_dragRow)
        {
            return;
        }
        const auto targetRow = _RowFromSender(sender);
        if (!targetRow || !targetRow.Workspace())
        {
            return;
        }

        if (_dragRow.Kind() == TerminalApp::WorkspaceSidebarRowKind::Workspace)
        {
            if (_dragRow.Workspace() && _dragRow.Workspace() != targetRow.Workspace())
            {
                args.AcceptedOperation(DataPackageOperation::Move);
                args.Handled(true);
            }
        }
        else if (_dragRow.Kind() == TerminalApp::WorkspaceSidebarRowKind::Surface)
        {
            if (_dragRow.Surface() && _dragRow.Surface() != targetRow.Surface())
            {
                args.AcceptedOperation(DataPackageOperation::Move);
                args.Handled(true);
            }
        }
    }

    void WorkspaceSidebar::_TreeRow_Drop(const IInspectable& sender, const DragEventArgs& args)
    {
        const auto dragRow = std::exchange(_dragRow, nullptr);
        if (!dragRow || !_workspaces)
        {
            return;
        }
        const auto targetRow = _RowFromSender(sender);
        if (!targetRow || !targetRow.Workspace())
        {
            return;
        }

        if (dragRow.Kind() == TerminalApp::WorkspaceSidebarRowKind::Workspace && dragRow.Workspace())
        {
            uint32_t targetIndex{};
            if (_workspaces.IndexOf(targetRow.Workspace(), targetIndex))
            {
                ReorderWorkspaceRequested.raise(dragRow.Workspace(), targetIndex);
                args.Handled(true);
            }
        }
        else if (dragRow.Kind() == TerminalApp::WorkspaceSidebarRowKind::Surface && dragRow.Surface())
        {
            std::optional<uint32_t> targetSurfaceIndex;
            if (targetRow.Surface())
            {
                uint32_t idx{};
                if (targetRow.Workspace().Surfaces().IndexOf(targetRow.Surface(), idx))
                {
                    targetSurfaceIndex = idx;
                }
            }
            _treeModel.SetWorkspaceCollapsed(targetRow.Workspace().WorkspaceId(), false);
            MoveSurfaceRequested.raise(dragRow.Surface(), targetRow.Workspace(), targetSurfaceIndex);
            args.Handled(true);
        }
    }

    void WorkspaceSidebar::_SetWorkspaceItemHoverVisibility(const IInspectable& sender, const Windows::UI::Xaml::Visibility visibility) const
    {
        if (const auto itemRoot = sender.try_as<FrameworkElement>())
        {
            if (const auto actions = itemRoot.FindName(L"WorkspaceRowHoverActions").try_as<UIElement>())
            {
                actions.Visibility(visibility);
            }
            if (const auto countBadge = itemRoot.FindName(L"WorkspaceRowCountBadge").try_as<UIElement>())
            {
                countBadge.Visibility(visibility == Windows::UI::Xaml::Visibility::Visible ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible);
            }
        }
    }

    void WorkspaceSidebar::_SetSurfaceItemHoverVisibility(const IInspectable& sender, const Windows::UI::Xaml::Visibility visibility) const
    {
        if (const auto itemRoot = sender.try_as<FrameworkElement>())
        {
            if (const auto closeBtn = itemRoot.FindName(L"SurfaceRowCloseButton").try_as<UIElement>())
            {
                closeBtn.Visibility(visibility);
            }
        }
    }

    void WorkspaceSidebar::_WorkspaceItem_PointerEntered(const IInspectable& sender, const Input::PointerRoutedEventArgs&)
    {
        _SetWorkspaceItemHoverVisibility(sender, Visibility::Visible);
    }

    void WorkspaceSidebar::_WorkspaceItem_PointerExited(const IInspectable& sender, const Input::PointerRoutedEventArgs& args)
    {
        if (const auto itemRoot = sender.try_as<FrameworkElement>())
        {
            const auto position = args.GetCurrentPoint(itemRoot).Position();
            if (position.X < 0.0 || position.Y < 0.0 ||
                position.X > itemRoot.ActualWidth() || position.Y > itemRoot.ActualHeight())
            {
                _SetWorkspaceItemHoverVisibility(sender, Visibility::Collapsed);
            }
        }
    }

    void WorkspaceSidebar::_WorkspaceItemCloseButton_PointerExited(const IInspectable& sender, const Input::PointerRoutedEventArgs& args)
    {
        if (const auto button = sender.try_as<FrameworkElement>())
        {
            if (const auto stack = button.Parent().try_as<FrameworkElement>())
            {
                if (const auto grid = stack.Parent().try_as<FrameworkElement>())
                {
                    if (const auto itemRoot = grid.Parent().try_as<FrameworkElement>())
                    {
                        const auto position = args.GetCurrentPoint(itemRoot).Position();
                        if (position.X < 0.0 || position.Y < 0.0 ||
                            position.X > itemRoot.ActualWidth() || position.Y > itemRoot.ActualHeight())
                        {
                            _SetWorkspaceItemHoverVisibility(itemRoot, Visibility::Collapsed);
                        }
                    }
                }
            }
        }
    }

    void WorkspaceSidebar::_SurfaceItem_PointerEntered(const IInspectable& sender, const Input::PointerRoutedEventArgs&)
    {
        _SetSurfaceItemHoverVisibility(sender, Visibility::Visible);
    }

    void WorkspaceSidebar::_SurfaceItem_PointerExited(const IInspectable& sender, const Input::PointerRoutedEventArgs& args)
    {
        if (const auto itemRoot = sender.try_as<FrameworkElement>())
        {
            const auto position = args.GetCurrentPoint(itemRoot).Position();
            if (position.X < 0.0 || position.Y < 0.0 ||
                position.X > itemRoot.ActualWidth() || position.Y > itemRoot.ActualHeight())
            {
                _SetSurfaceItemHoverVisibility(sender, Visibility::Collapsed);
            }
        }
    }

    void WorkspaceSidebar::_Chevron_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ToggleWorkspaceCollapsed(item.WorkspaceId());
        }
    }

    void WorkspaceSidebar::_AddSurface_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            _treeModel.SetWorkspaceCollapsed(item.WorkspaceId(), false);
            NewSurfaceRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_OpenDirectory_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            const auto dir = item.WorkingDirectory();
            if (!dir.empty())
            {
                ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }

    void WorkspaceSidebar::_CopyPath_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            const auto dir = item.WorkingDirectory();
            if (!dir.empty())
            {
                DataPackage dataPackage{};
                dataPackage.SetText(dir);
                Clipboard::SetContent(dataPackage);
                Clipboard::Flush();
            }
        }
    }

    void WorkspaceSidebar::_CloseSurface_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto surface = _SurfaceFromSender(sender))
        {
            CloseSurfaceRequested.raise(*this, surface);
        }
    }

    void WorkspaceSidebar::_RenameSurface_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto surface = _SurfaceFromSender(sender))
        {
            RenameSurfaceRequested.raise(*this, surface);
        }
    }

    void WorkspaceSidebar::_SplitSurface_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto surface = _SurfaceFromSender(sender))
        {
            SplitSurfaceRequested.raise(*this, surface);
        }
    }

    void WorkspaceSidebar::_Pin_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            PinRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_Close_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            CloseRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_CloseOthers_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            CloseOthersRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_Rename_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            RenameRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_SaveSnapshot_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            SaveSnapshotRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_ConfigureAgentResume_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ConfigureAgentResumeRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_ResumeAgent_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ResumeAgentRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_ClearAgentResume_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ClearAgentResumeRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_Notification_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ActivateNotificationRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_MarkNotificationsRead_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            MarkNotificationsReadRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_ClearNotificationHistory_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto item = _ItemFromSender(sender))
        {
            ClearNotificationHistoryRequested.raise(*this, item);
        }
    }

    void WorkspaceSidebar::_NotificationCenter_Opening(const IInspectable&, const IInspectable&)
    {
        RefreshNotifications();
    }

    void WorkspaceSidebar::_NotificationHistory_ItemClick(const IInspectable&, const ItemClickEventArgs& args)
    {
        if (const auto notification = args.ClickedItem().try_as<TerminalApp::WorkspaceNotification>())
        {
            _ActivateNotification(notification);
        }
    }

    void WorkspaceSidebar::_NotificationHistory_KeyDown(const IInspectable&, const Input::KeyRoutedEventArgs& args)
    {
        const auto key = args.OriginalKey();
        if (key != Windows::System::VirtualKey::Enter && key != Windows::System::VirtualKey::Space)
        {
            return;
        }

        if (const auto element = args.OriginalSource().try_as<FrameworkElement>())
        {
            if (const auto notification = element.DataContext().try_as<TerminalApp::WorkspaceNotification>())
            {
                _ActivateNotification(notification);
                args.Handled(true);
            }
        }
    }

    void WorkspaceSidebar::_ActivateNotification(const TerminalApp::WorkspaceNotification& notification)
    {
        NotificationCenterFlyout().Hide();
        ActivateNotificationItemRequested.raise(*this, notification);
    }

    void WorkspaceSidebar::_MarkAllNotificationsRead_Click(const IInspectable&, const RoutedEventArgs&)
    {
        MarkAllNotificationsReadRequested.raise(*this, nullptr);
    }

    void WorkspaceSidebar::_ClearAllNotificationHistory_Click(const IInspectable&, const RoutedEventArgs&)
    {
        ClearAllNotificationHistoryRequested.raise(*this, nullptr);
    }

    void WorkspaceSidebar::_RemoteBridgeFlyout_Opening(const IInspectable&, const IInspectable&)
    {
        if (_layoutElementsReady)
        {
            RemoteBridgeCopyFeedbackText().Visibility(Visibility::Collapsed);
            _ApplyRemoteBridgeStatus(_remoteBridgeStatus, _remoteBridgeBusy);
        }
        _RefreshRemoteBridgeAsync(false);
    }

    void WorkspaceSidebar::_RemoteBridgeToggle_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _ToggleRemoteBridgeAsync();
    }

    void WorkspaceSidebar::_RemoteBridgeRefresh_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _RefreshRemoteBridgeAsync(true);
    }

    void WorkspaceSidebar::_RemoteBridgeCopyUri_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (_remoteBridgeStatus.PairUri.empty())
        {
            return;
        }
        DataPackage dataPackage{};
        dataPackage.SetText(_remoteBridgeStatus.PairUri);
        Clipboard::SetContent(dataPackage);
        Clipboard::Flush();

        if (_layoutElementsReady)
        {
            RemoteBridgeCopyFeedbackText().Text(RS_(L"WorkspaceSidebar_RemoteCopied"));
            RemoteBridgeCopyFeedbackText().Visibility(Visibility::Visible);
        }
    }

    void WorkspaceSidebar::_RemoteBridgeAutoStart_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (!_layoutElementsReady)
        {
            return;
        }
        const auto checked = RemoteBridgeAutoStartCheckBox().IsChecked();
        const bool enabled = checked && checked.Value();
        WorkspaceRemoteBridge::SaveAutoStartPreference(enabled);
        if (enabled && !_remoteBridgeStatus.Running && !_remoteBridgeBusy)
        {
            _ToggleRemoteBridgeAsync();
        }
    }

    void WorkspaceSidebar::_RemoteBridgeHooks_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _ToggleRemoteHooksAsync();
    }

    void WorkspaceSidebar::_RemoteBridgeTunnel_Click(const IInspectable&, const RoutedEventArgs&)
    {
        _ToggleRemoteTunnelAsync();
    }

    void WorkspaceSidebar::_RemoteBridgeRevokeDevice_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto btn = sender.try_as<FrameworkElement>())
        {
            if (const auto id = btn.Tag().try_as<winrt::hstring>(); id && !id->empty())
            {
                _RevokeRemoteDeviceAsync(std::wstring{ *id });
            }
        }
    }

    winrt::fire_and_forget WorkspaceSidebar::_AutoStartRemoteBridgeAsync()
    {
        if (_remoteBridgeBusy)
        {
            co_return;
        }
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        if (!dispatcher)
        {
            co_return;
        }

        const auto generation = ++_remoteBridgeGeneration;
        _remoteBridgeBusy = true;
        _ApplyRemoteBridgeStatus(_remoteBridgeStatus, true);

        co_await winrt::resume_background();

        RemoteBridgeStatus nextStatus{};
        try
        {
            if (const auto self = weakThis.get())
            {
                std::wstring startError;
                if (self->_remoteBridge.Start(startError, WorkspaceRemoteBridge::DefaultPort))
                {
                    for (int attempt = 0; attempt < 20; ++attempt)
                    {
                        nextStatus = self->_remoteBridge.RefreshStatus(WorkspaceRemoteBridge::DefaultPort, false);
                        if (nextStatus.Running)
                        {
                            break;
                        }
                        Sleep(150);
                    }
                }
                else if (!startError.empty())
                {
                    nextStatus.ErrorMessage = std::move(startError);
                }
            }
        }
        CATCH_LOG();

        co_await wil::resume_foreground(dispatcher);
        if (const auto self = weakThis.get())
        {
            if (self->_remoteBridgeGeneration == generation)
            {
                self->_remoteBridgeBusy = false;
                self->_remoteBridgeStatus = std::move(nextStatus);
                self->_ApplyRemoteBridgeStatus(self->_remoteBridgeStatus, false);
            }
        }
    }

    winrt::fire_and_forget WorkspaceSidebar::_ToggleRemoteHooksAsync()
    {
        if (_remoteBridgeBusy || !_remoteBridgeStatus.Running)
        {
            co_return;
        }
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        if (!dispatcher)
        {
            co_return;
        }

        const bool shouldInstall = !(_remoteBridgeStatus.ClaudeHookInstalled && _remoteBridgeStatus.CodexHookInstalled);
        const auto generation = ++_remoteBridgeGeneration;
        _remoteBridgeBusy = true;
        _ApplyRemoteBridgeStatus(_remoteBridgeStatus, true);

        co_await winrt::resume_background();

        RemoteBridgeStatus nextStatus{};
        try
        {
            if (const auto self = weakThis.get())
            {
                nextStatus = self->_remoteBridge.InstallHooks(shouldInstall, WorkspaceRemoteBridge::DefaultPort);
            }
        }
        CATCH_LOG();

        co_await wil::resume_foreground(dispatcher);
        if (const auto self = weakThis.get())
        {
            if (self->_remoteBridgeGeneration == generation)
            {
                self->_remoteBridgeBusy = false;
                self->_remoteBridgeStatus = std::move(nextStatus);
                self->_ApplyRemoteBridgeStatus(self->_remoteBridgeStatus, false);
            }
        }
    }

    winrt::fire_and_forget WorkspaceSidebar::_ToggleRemoteTunnelAsync()
    {
        if (_remoteBridgeBusy || !_remoteBridgeStatus.Running)
        {
            co_return;
        }
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        if (!dispatcher)
        {
            co_return;
        }

        const bool shouldEnable = !_remoteBridgeStatus.TunnelRunning;
        const auto generation = ++_remoteBridgeGeneration;
        _remoteBridgeBusy = true;
        _ApplyRemoteBridgeStatus(_remoteBridgeStatus, true);

        co_await winrt::resume_background();

        RemoteBridgeStatus nextStatus{};
        try
        {
            if (const auto self = weakThis.get())
            {
                nextStatus = self->_remoteBridge.ToggleTunnel(shouldEnable, WorkspaceRemoteBridge::DefaultPort);
            }
        }
        CATCH_LOG();

        co_await wil::resume_foreground(dispatcher);
        if (const auto self = weakThis.get())
        {
            if (self->_remoteBridgeGeneration == generation)
            {
                self->_remoteBridgeBusy = false;
                self->_remoteBridgeStatus = std::move(nextStatus);
                self->_ApplyRemoteBridgeStatus(self->_remoteBridgeStatus, false);
            }
        }
    }

    winrt::fire_and_forget WorkspaceSidebar::_RevokeRemoteDeviceAsync(std::wstring deviceId)
    {
        if (_remoteBridgeBusy || !_remoteBridgeStatus.Running || deviceId.empty())
        {
            co_return;
        }
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        if (!dispatcher)
        {
            co_return;
        }

        const auto generation = ++_remoteBridgeGeneration;
        _remoteBridgeBusy = true;
        _ApplyRemoteBridgeStatus(_remoteBridgeStatus, true);

        co_await winrt::resume_background();

        RemoteBridgeStatus nextStatus{};
        try
        {
            if (const auto self = weakThis.get())
            {
                nextStatus = self->_remoteBridge.RevokeDevice(deviceId, WorkspaceRemoteBridge::DefaultPort);
            }
        }
        CATCH_LOG();

        co_await wil::resume_foreground(dispatcher);
        if (const auto self = weakThis.get())
        {
            if (self->_remoteBridgeGeneration == generation)
            {
                self->_remoteBridgeBusy = false;
                self->_remoteBridgeStatus = std::move(nextStatus);
                self->_ApplyRemoteBridgeStatus(self->_remoteBridgeStatus, false);
            }
        }
    }

    winrt::fire_and_forget WorkspaceSidebar::_RefreshRemoteBridgeAsync(const bool refreshCode)
    {
        if (_remoteBridgeBusy)
        {
            co_return;
        }
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        if (!dispatcher)
        {
            co_return;
        }

        const auto generation = ++_remoteBridgeGeneration;
        co_await winrt::resume_background();

        RemoteBridgeStatus nextStatus{};
        try
        {
            if (const auto self = weakThis.get())
            {
                nextStatus = self->_remoteBridge.RefreshStatus(WorkspaceRemoteBridge::DefaultPort, refreshCode);
            }
        }
        CATCH_LOG();

        co_await wil::resume_foreground(dispatcher);
        if (const auto self = weakThis.get())
        {
            if (self->_remoteBridgeGeneration == generation && !self->_remoteBridgeBusy)
            {
                self->_remoteBridgeStatus = std::move(nextStatus);
                self->_ApplyRemoteBridgeStatus(self->_remoteBridgeStatus, self->_remoteBridgeBusy);
            }
        }
    }

    winrt::fire_and_forget WorkspaceSidebar::_ToggleRemoteBridgeAsync()
    {
        if (_remoteBridgeBusy)
        {
            co_return;
        }

        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        if (!dispatcher)
        {
            co_return;
        }

        const auto generation = ++_remoteBridgeGeneration;
        _remoteBridgeBusy = true;
        _ApplyRemoteBridgeStatus(_remoteBridgeStatus, true);

        const bool shouldStop = _remoteBridgeStatus.Running;
        co_await winrt::resume_background();

        RemoteBridgeStatus nextStatus{};
        try
        {
            if (const auto self = weakThis.get())
            {
                if (shouldStop)
                {
                    if (self->_remoteBridge.IsOwnedRunning())
                    {
                        self->_remoteBridge.Stop();
                        nextStatus = self->_remoteBridge.RefreshStatus(WorkspaceRemoteBridge::DefaultPort, false);
                    }
                    else
                    {
                        nextStatus = self->_remoteBridge.RefreshStatus(WorkspaceRemoteBridge::DefaultPort, false);
                        if (nextStatus.Running)
                        {
                            nextStatus.ErrorMessage = std::wstring{ RS_(L"WorkspaceSidebar_RemoteExternalStopHint") };
                        }
                    }
                }
                else
                {
                    std::wstring startError;
                    if (!self->_remoteBridge.Start(startError, WorkspaceRemoteBridge::DefaultPort))
                    {
                        nextStatus.ErrorMessage = std::move(startError);
                    }
                    else
                    {
                        // Poll briefly until the newly spawned Node.js sidecar opens its HTTPS listener.
                        for (int attempt = 0; attempt < 20; ++attempt)
                        {
                            nextStatus = self->_remoteBridge.RefreshStatus(WorkspaceRemoteBridge::DefaultPort, false);
                            if (nextStatus.Running)
                            {
                                break;
                            }
                            Sleep(150);
                        }
                        if (!nextStatus.Running && nextStatus.ErrorMessage.empty())
                        {
                            nextStatus.ErrorMessage = L"Remote Bridge started but did not respond on 127.0.0.1:47710.";
                        }
                    }
                }
            }
        }
        CATCH_LOG();

        co_await wil::resume_foreground(dispatcher);
        if (const auto self = weakThis.get())
        {
            if (self->_remoteBridgeGeneration == generation)
            {
                self->_remoteBridgeBusy = false;
                self->_remoteBridgeStatus = std::move(nextStatus);
                self->_ApplyRemoteBridgeStatus(self->_remoteBridgeStatus, false);
            }
        }
    }

    void WorkspaceSidebar::_ApplyRemoteBridgeStatus(const RemoteBridgeStatus& status, const bool busy)
    {
        if (!_layoutElementsReady)
        {
            return;
        }

        RemoteBridgeRunningDot().Visibility(status.Running ? Visibility::Visible : Visibility::Collapsed);
        RemoteBridgeToggleButton().IsEnabled(!busy);
        RemoteBridgeRefreshButton().IsEnabled(!busy && status.Running);

        if (busy)
        {
            RemoteBridgeStatusText().Text(RS_(L"WorkspaceSidebar_RemoteStarting"));
            RemoteBridgeToggleButton().Content(box_value(status.Running ? RS_(L"WorkspaceSidebar_RemoteStop") : RS_(L"WorkspaceSidebar_RemoteStart")));
        }
        else if (status.Running)
        {
            RemoteBridgeStatusText().Text(RS_(L"WorkspaceSidebar_RemoteRunning"));
            RemoteBridgeToggleButton().Content(box_value(RS_(L"WorkspaceSidebar_RemoteStop")));
        }
        else
        {
            RemoteBridgeStatusText().Text(RS_(L"WorkspaceSidebar_RemoteStopped"));
            RemoteBridgeToggleButton().Content(box_value(RS_(L"WorkspaceSidebar_RemoteStart")));
        }

        RemoteBridgeRunningPanel().Visibility(status.Running ? Visibility::Visible : Visibility::Collapsed);
        RemoteBridgeStoppedPanel().Visibility(status.Running ? Visibility::Collapsed : Visibility::Visible);

        if (status.Running)
        {
            RemoteBridgeCodeText().Text(status.PairingCode);
            RemoteBridgeLanUrlText().Text(status.LanUrl);
            RemoteBridgeRemoteUrlText().Text(status.RemoteUrl.empty() ? RS_(L"WorkspaceSidebar_RemoteNoIpv6") : winrt::hstring{ status.RemoteUrl });

            // Render crisp QR code bitmap when matrix changes.
            if (status.QrSize >= 21 && status.QrModules.size() == static_cast<size_t>(status.QrSize) * status.QrSize)
            {
                if (_renderedQrKey != status.QrModules)
                {
                    constexpr uint32_t quiet = 2;
                    const uint32_t totalModules = status.QrSize + quiet * 2;
                    const uint32_t scale = std::max<uint32_t>(3u, 168u / totalModules);
                    const uint32_t dim = totalModules * scale;

                    Windows::UI::Xaml::Media::Imaging::WriteableBitmap bitmap{ gsl::narrow_cast<int32_t>(dim), gsl::narrow_cast<int32_t>(dim) };
                    auto pixelBuffer = bitmap.PixelBuffer();
                    if (const auto byteAccess = pixelBuffer.try_as<::Windows::Storage::Streams::IBufferByteAccess>())
                    {
                        uint8_t* bytes = nullptr;
                        if (SUCCEEDED(byteAccess->Buffer(&bytes)) && bytes != nullptr)
                        {
                            // Fill white background (BGRA = FF FF FF FF).
                            std::memset(bytes, 0xFF, static_cast<size_t>(dim) * dim * 4);
                            for (uint32_t r = 0; r < status.QrSize; ++r)
                            {
                                for (uint32_t c = 0; c < status.QrSize; ++c)
                                {
                                    if (status.QrModules[static_cast<size_t>(r) * status.QrSize + c] != '1')
                                    {
                                        continue;
                                    }
                                    const uint32_t y0 = (r + quiet) * scale;
                                    const uint32_t x0 = (c + quiet) * scale;
                                    for (uint32_t dy = 0; dy < scale; ++dy)
                                    {
                                        uint8_t* rowPtr = bytes + (static_cast<size_t>(y0 + dy) * dim + x0) * 4;
                                        for (uint32_t dx = 0; dx < scale; ++dx)
                                        {
                                            rowPtr[dx * 4 + 0] = 0x00;
                                            rowPtr[dx * 4 + 1] = 0x00;
                                            rowPtr[dx * 4 + 2] = 0x00;
                                            rowPtr[dx * 4 + 3] = 0xFF;
                                        }
                                    }
                                }
                            }
                            bitmap.Invalidate();
                            RemoteBridgeQrImage().Source(bitmap);
                            _renderedQrKey = status.QrModules;
                        }
                    }
                }
            }

            if (!status.CertFingerprint.empty())
            {
                const auto preview = status.CertFingerprint.size() > 24 ?
                                         status.CertFingerprint.substr(0, 24) + L"…" :
                                         status.CertFingerprint;
                RemoteBridgeTlsFingerprintText().Text(preview);
                ToolTipService::SetToolTip(RemoteBridgeTlsFingerprintText(), box_value(status.CertFingerprint));
                RemoteBridgeTlsFingerprintText().Visibility(Visibility::Visible);
            }
            else
            {
                RemoteBridgeTlsFingerprintText().Text(L"HTTP (TLS disabled)");
                ToolTipService::SetToolTip(RemoteBridgeTlsFingerprintText(), nullptr);
                RemoteBridgeTlsFingerprintText().Visibility(Visibility::Visible);
            }

            // Hooks status & one-click button
            const bool allHooks = status.ClaudeHookInstalled && status.CodexHookInstalled;
            RemoteBridgeHooksStatusText().Text(
                allHooks ? RS_(L"WorkspaceSidebar_RemoteHooksStatusReady") :
                           winrt::hstring{ RS_fmt(L"WorkspaceSidebar_RemoteHooksStatusPartial",
                                                  status.ClaudeHookInstalled ? L"✓" : L"–",
                                                  status.CodexHookInstalled ? L"✓" : L"–") });
            RemoteBridgeHooksButton().Content(box_value(allHooks ? RS_(L"WorkspaceSidebar_RemoteHooksUninstall") : RS_(L"WorkspaceSidebar_RemoteHooksInstall")));
            RemoteBridgeHooksButton().IsEnabled(!busy);

            // Public Quick Tunnel status & toggle
            if (status.TunnelRunning && !status.TunnelPublicUrl.empty())
            {
                RemoteBridgeTunnelStatusText().Text(status.TunnelPublicUrl);
                RemoteBridgeTunnelButton().Content(box_value(RS_(L"WorkspaceSidebar_RemoteTunnelStop")));
                RemoteBridgeTunnelButton().IsEnabled(!busy);
            }
            else if (status.TunnelAvailable)
            {
                RemoteBridgeTunnelStatusText().Text(RS_(L"WorkspaceSidebar_RemoteTunnelReady"));
                RemoteBridgeTunnelButton().Content(box_value(RS_(L"WorkspaceSidebar_RemoteTunnelStart")));
                RemoteBridgeTunnelButton().IsEnabled(!busy);
            }
            else
            {
                RemoteBridgeTunnelStatusText().Text(RS_(L"WorkspaceSidebar_RemoteTunnelMissing"));
                RemoteBridgeTunnelButton().Content(box_value(RS_(L"WorkspaceSidebar_RemoteTunnelStart")));
                RemoteBridgeTunnelButton().IsEnabled(false);
            }

            // Paired devices list with Revoke buttons
            const auto devicesList = RemoteBridgeDevicesList();
            devicesList.Children().Clear();
            if (!status.Devices.empty())
            {
                RemoteBridgeDevicesSection().Visibility(Visibility::Visible);
                for (const auto& dev : status.Devices)
                {
                    Grid row{};
                    ColumnDefinition col0{};
                    col0.Width(GridLengthHelper::FromValueAndType(1.0, GridUnitType::Star));
                    ColumnDefinition col1{};
                    col1.Width(GridLengthHelper::Auto());
                    row.ColumnDefinitions().Append(col0);
                    row.ColumnDefinitions().Append(col1);

                    TextBlock nameText{};
                    nameText.Text(dev.Name);
                    nameText.FontSize(11.5);
                    nameText.VerticalAlignment(VerticalAlignment::Center);
                    nameText.TextTrimming(TextTrimming::CharacterEllipsis);
                    Grid::SetColumn(nameText, 0);

                    Button revokeBtn{};
                    revokeBtn.Content(box_value(RS_(L"WorkspaceSidebar_RemoteDeviceRevoke")));
                    revokeBtn.FontSize(10.5);
                    revokeBtn.Height(24);
                    revokeBtn.Padding(Thickness{ 8, 0, 8, 0 });
                    revokeBtn.IsEnabled(!busy);
                    revokeBtn.Tag(box_value(winrt::hstring{ dev.Id }));
                    revokeBtn.Click({ get_weak(), &WorkspaceSidebar::_RemoteBridgeRevokeDevice_Click });
                    Grid::SetColumn(revokeBtn, 1);

                    row.Children().Append(nameText);
                    row.Children().Append(revokeBtn);
                    devicesList.Children().Append(row);
                }
            }
            else
            {
                RemoteBridgeDevicesSection().Visibility(Visibility::Collapsed);
            }

            RemoteBridgePairedCountText().Text(RS_fmt(L"WorkspaceSidebar_RemotePairedDevices", status.PairedDeviceCount));
            RemoteBridgeCopyUriButton().IsEnabled(!status.PairUri.empty());
        }

        if (!status.ErrorMessage.empty())
        {
            RemoteBridgeErrorText().Text(status.ErrorMessage);
            RemoteBridgeErrorText().Visibility(Visibility::Visible);
        }
        else
        {
            RemoteBridgeErrorText().Text(L"");
            RemoteBridgeErrorText().Visibility(Visibility::Collapsed);
        }
    }
}
