// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"
#include "WorkspaceFeatureSet.h"

#include <TerminalCore/ControlKeyStates.hpp>
#include <TerminalThemeHelpers.h>
#include <til/hash.h>
#include <til/unicode.h>
#include <Utils.h>

#include "../../types/inc/ColorFix.hpp"
#include "../../types/inc/utils.hpp"
#include "../TerminalSettingsAppAdapterLib/TerminalSettings.h"
#include "App.h"
#include "DebugTapConnection.h"
#include "MarkdownPaneContent.h"
#include "Remoting.h"
#include "ScratchpadContent.h"
#include "SettingsPaneContent.h"
#include "SnippetsPaneContent.h"
#include "TabRowControl.h"
#include "TerminalSettingsCache.h"
#include "WorkspaceListeningPorts.h"

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Core;
using namespace std::chrono_literals;

#define HOOKUP_ACTION(action) _actionDispatch->action({ this, &TerminalPage::_Handle##action });

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

namespace winrt::TerminalApp::implementation
{    void TerminalPage::_UpdateWorkspaceSidebar()
    {
        if (!_workspaceSidebar || !_workspaceList)
        {
            return;
        }
        const auto enabled = _currentWindowSettings().TabLayout() == Settings::Model::TabLayout::WorkspaceSidebar;

        _tabRow.WorkspaceSidebarMode(enabled);

        // The titlebar keeps hosting TabRow in both layouts. In sidebar mode,
        // TabRow replaces the horizontal tabs with the compact workspace
        // controls while retaining the existing new-terminal flyout.
        if (_currentWindowSettings().ShowTabsInTitlebar() &&
            (!_SidebarController().LayoutInitialized || enabled != _SidebarController().Enabled))
        {
            uint32_t tabRowIndex{};
            const auto tabRowIsInPage = Root().Children().IndexOf(_tabRow, tabRowIndex);
            if (tabRowIsInPage)
            {
                Root().Children().RemoveAt(tabRowIndex);
            }
            SetTitleBarContent.raise(*this, _tabRow);
        }

        _SidebarController().Enabled = enabled;
        _SidebarController().LayoutInitialized = true;
        _conclusionSidebar.Visibility(enabled ? Visibility::Visible : Visibility::Collapsed);
        _workspaceFeatures->History().UpdateView();
        _UpdateWorkspaceTitleBar();
        _UpdateWorkspaceListeningPortsTimer();
        _workspaceFeatures->Automation().Update(_SidebarController().Enabled);
        _UpdateTabView();
    }


    void TerminalPage::_UpdateWorkspaceTitleBar()
    {
        if (!_tabRow || !_workspaceSidebarToggleButton || !_workspaceNotificationButton)
        {
            return;
        }

        _tabRow.WorkspaceSidebarMode(_SidebarController().Enabled);
        if (!_SidebarController().Enabled)
        {
            winrt::get_self<implementation::WorkspaceSidebar>(_workspaceSidebar)->ShowSwitchHints(false);
            return;
        }

        const auto sidebar = winrt::get_self<implementation::WorkspaceSidebar>(_workspaceSidebar);
        const auto expanded = sidebar->IsEffectivelyExpanded();
        const auto toggleName = sidebar->IsWindowNarrow() ?
                                    RS_(L"WorkspaceSidebar_NarrowAutomationName") :
                                    expanded ? RS_(L"WorkspaceSidebar_CollapseAutomationName") :
                                               RS_(L"WorkspaceSidebar_ExpandAutomationName");

        _workspaceSidebarToggleButton.Content(box_value(expanded ? L"\uE76B" : L"\uE76C"));
        _workspaceSidebarToggleButton.IsEnabled(!sidebar->IsWindowNarrow());
        Automation::AutomationProperties::SetName(_workspaceSidebarToggleButton, toggleName);
        ToolTipService::SetToolTip(_workspaceSidebarToggleButton, box_value(toggleName));

        const auto notificationName = RS_(L"WorkspaceSidebar_NotificationCenterAutomationName");
        Automation::AutomationProperties::SetName(_workspaceNotificationButton, notificationName);
        ToolTipService::SetToolTip(_workspaceNotificationButton, box_value(notificationName));
    }

    void TerminalPage::_UpdateWorkspaceListeningPortsTimer()
    {
        if (_SidebarController().Enabled)
        {
            if (!_SidebarController().ListeningPortsTimer.IsEnabled())
            {
                _SidebarController().ListeningPortsTimer.Start();
                _RefreshWorkspaceListeningPorts();
            }
            return;
        }

        _SidebarController().ListeningPortsTimer.Stop();
        _SidebarController().ListeningPortsRefreshPending = false;
        ++_SidebarController().ListeningPortsGeneration;
        for (const auto& item : _WorkspaceItems())
        {
            winrt::get_self<WorkspaceItemViewModel>(item)->UpdateListeningPorts({});
        }
    }

    void TerminalPage::_RefreshWorkspaceListeningPorts()
    {
        if (!_SidebarController().Enabled)
        {
            return;
        }
        if (_SidebarController().ListeningPortsRefreshInFlight)
        {
            _SidebarController().ListeningPortsRefreshPending = true;
            return;
        }

        _SidebarController().ListeningPortsRefreshPending = false;

        std::vector<WorkspaceProcessRoots> workspaces;
        std::vector<uint32_t> allRootProcessIds;
        workspaces.reserve(_WorkspaceItems().Size());

        for (const auto& item : _WorkspaceItems())
        {
            WorkspaceProcessRoots workspace{ item.WorkspaceId() };
            if (const auto tab = _GetTabImpl(item.SourceTab()); tab && tab->GetRootPane())
            {
                tab->GetRootPane()->WalkTree([&](const auto& pane) {
                    if (const auto control = pane->GetTerminalControl())
                    {
                        if (const auto connection = control.Connection())
                        {
                            if (const auto conpty = connection.try_as<TerminalConnection::ConptyConnection>())
                            {
                                const auto handle = reinterpret_cast<HANDLE>(conpty.RootProcessHandle());
                                if (handle)
                                {
                                    if (const auto processId = GetProcessId(handle); processId != 0)
                                    {
                                        workspace.RootProcessIds.emplace_back(processId);
                                        allRootProcessIds.emplace_back(processId);
                                    }
                                }
                            }
                        }
                    }
                });
            }

            std::sort(workspace.RootProcessIds.begin(), workspace.RootProcessIds.end());
            workspace.RootProcessIds.erase(std::unique(workspace.RootProcessIds.begin(), workspace.RootProcessIds.end()),
                                           workspace.RootProcessIds.end());
            workspaces.emplace_back(std::move(workspace));
        }

        std::sort(allRootProcessIds.begin(), allRootProcessIds.end());
        allRootProcessIds.erase(std::unique(allRootProcessIds.begin(), allRootProcessIds.end()), allRootProcessIds.end());
        const auto generation = ++_SidebarController().ListeningPortsGeneration;
        if (allRootProcessIds.empty())
        {
            for (const auto& item : _WorkspaceItems())
            {
                winrt::get_self<WorkspaceItemViewModel>(item)->UpdateListeningPorts({});
            }
            return;
        }

        _SidebarController().ListeningPortsRefreshInFlight = true;
        _RefreshWorkspaceListeningPortsAsync(generation, std::move(workspaces), std::move(allRootProcessIds));
    }

    safe_void_coroutine TerminalPage::_RefreshWorkspaceListeningPortsAsync(const uint64_t generation,
                                                                           std::vector<WorkspaceProcessRoots> workspaces,
                                                                           std::vector<uint32_t> rootProcessIds)
    {
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        co_await winrt::resume_background();
        const auto snapshot = WorkspaceListeningPorts::Query(rootProcessIds);
        co_await wil::resume_foreground(dispatcher);

        if (const auto page = weakThis.get())
        {
            page->_SidebarController().ListeningPortsRefreshInFlight = false;
            const auto refreshPending = std::exchange(page->_SidebarController().ListeningPortsRefreshPending, false);
            if (snapshot.Succeeded &&
                generation == page->_SidebarController().ListeningPortsGeneration &&
                page->_SidebarController().Enabled)
            {
                for (const auto& workspace : workspaces)
                {
                    std::vector<uint16_t> ports;
                    for (const auto processId : workspace.RootProcessIds)
                    {
                        if (const auto entry = snapshot.Ports.find(processId); entry != snapshot.Ports.end())
                        {
                            ports.insert(ports.end(), entry->second.begin(), entry->second.end());
                        }
                    }
                    std::sort(ports.begin(), ports.end());
                    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());

                    for (const auto& item : page->_WorkspaceItems())
                    {
                        if (item.WorkspaceId() == workspace.WorkspaceId)
                        {
                            winrt::get_self<WorkspaceItemViewModel>(item)->UpdateListeningPorts(std::move(ports));
                            break;
                        }
                    }
                }
            }

            if (refreshPending)
            {
                page->_RefreshWorkspaceListeningPorts();
            }
        }
    }

    void TerminalPage::_UpdateWorkspaceSidebarForWindowWidth(const double width)
    {
        if (_workspaceSidebar)
        {
            const auto sidebar = winrt::get_self<implementation::WorkspaceSidebar>(_workspaceSidebar);
            sidebar->SetWindowNarrow(width < implementation::WorkspaceSidebar::NarrowWindowThreshold);
            _UpdateWorkspaceTitleBar();
        }
        if (_conclusionSidebar)
        {
            winrt::get_self<implementation::ConclusionSidebar>(_conclusionSidebar)->SetWindowNarrow(
                width < implementation::ConclusionSidebar::NarrowWindowThreshold);
        }
    }

    void TerminalPage::_Root_SizeChanged(const IInspectable&, const SizeChangedEventArgs& args)
    {
        _UpdateWorkspaceSidebarForWindowWidth(args.NewSize().Width);
    }

    void TerminalPage::_SyncWorkspaceSelection(const winrt::TerminalApp::Tab& tab)
    {
        if (!_workspaceList || !tab)
        {
            return;
        }

        uint32_t tabIndex{};
        if (!_tabs.IndexOf(tab, tabIndex))
        {
            return;
        }

        for (uint32_t index = 0; index < _WorkspaceItems().Size(); ++index)
        {
            _WorkspaceItems().GetAt(index).IsSelected(index == tabIndex);
        }

        const auto selectedIndex = gsl::narrow_cast<int32_t>(tabIndex);
        if (_workspaceList.SelectedIndex() != selectedIndex)
        {
            _SidebarController().SyncingSelection = true;
            _workspaceList.SelectedIndex(selectedIndex);
            _SidebarController().SyncingSelection = false;
        }

        const auto item = _WorkspaceItems().GetAt(tabIndex);
        if (const auto container = _workspaceList.ContainerFromItem(item).try_as<FrameworkElement>())
        {
            container.StartBringIntoView();
        }
    }

    std::optional<uint32_t> TerminalPage::_FindWorkspaceIndex(const winrt::guid& workspaceId) const
    {
        return _workspaceFeatures->Workspaces().IndexOf(workspaceId);
    }

    void TerminalPage::_RecordWorkspaceNavigation(const winrt::TerminalApp::Tab& tab)
    {
        uint32_t tabIndex{};
        if (!tab || !_tabs.IndexOf(tab, tabIndex) || tabIndex >= _WorkspaceItems().Size())
        {
            return;
        }

        const auto workspaceId = _WorkspaceItems().GetAt(tabIndex).WorkspaceId();
        if (!_SidebarController().NavigationCurrent)
        {
            _SidebarController().NavigationCurrent = workspaceId;
            return;
        }
        if (_SidebarController().NavigationCurrent.value() == workspaceId)
        {
            return;
        }

        if (_FindWorkspaceIndex(_SidebarController().NavigationCurrent.value()))
        {
            _SidebarController().BackHistory.emplace_back(_SidebarController().NavigationCurrent.value());
            if (_SidebarController().BackHistory.size() > WorkspaceSidebarController::MaximumNavigationHistory)
            {
                _SidebarController().BackHistory.erase(_SidebarController().BackHistory.begin());
            }
        }

        _SidebarController().NavigationCurrent = workspaceId;
        _SidebarController().ForwardHistory.clear();
    }

    bool TerminalPage::_NavigateWorkspaceHistory(const bool forward)
    {
        auto& source = forward ? _SidebarController().ForwardHistory : _SidebarController().BackHistory;
        auto& destination = forward ? _SidebarController().BackHistory : _SidebarController().ForwardHistory;

        while (!source.empty())
        {
            const auto target = source.back();
            source.pop_back();
            if ((_SidebarController().NavigationCurrent && _SidebarController().NavigationCurrent.value() == target) || !_FindWorkspaceIndex(target))
            {
                continue;
            }

            if (_SidebarController().NavigationCurrent && _FindWorkspaceIndex(_SidebarController().NavigationCurrent.value()))
            {
                destination.emplace_back(_SidebarController().NavigationCurrent.value());
                if (destination.size() > WorkspaceSidebarController::MaximumNavigationHistory)
                {
                    destination.erase(destination.begin());
                }
            }

            _SidebarController().NavigationCurrent = target;
            return _SelectTab(_FindWorkspaceIndex(target).value());
        }
        return false;
    }

    uint32_t TerminalPage::_PinnedTabCount() const
    {
        uint32_t count = 0;
        for (const auto& tab : _tabs)
        {
            if (tab.IsPinned())
            {
                ++count;
            }
        }
        return count;
    }

    int32_t TerminalPage::_ClampTabIndexForPinnedState(const winrt::TerminalApp::Tab& tab, const int32_t suggestedIndex) const
    {
        const auto pinnedCount = gsl::narrow_cast<int32_t>(_PinnedTabCount());
        const auto lastIndex = gsl::narrow_cast<int32_t>(_tabs.Size()) - 1;
        const auto minimumIndex = tab.IsPinned() ? 0 : pinnedCount;
        const auto maximumIndex = tab.IsPinned() ? pinnedCount - 1 : lastIndex;
        return std::clamp(suggestedIndex, minimumIndex, maximumIndex);
    }

    void TerminalPage::_ToggleWorkspacePin(const winrt::TerminalApp::Tab& tab)
    {
        uint32_t currentIndex{};
        if (!tab || !_tabs.IndexOf(tab, currentIndex))
        {
            return;
        }

        tab.IsPinned(!tab.IsPinned());
        const auto targetIndex = tab.IsPinned() ? _PinnedTabCount() - 1 : _PinnedTabCount();
        _TryMoveTab(currentIndex, gsl::narrow_cast<int32_t>(targetIndex), false);
    }

    void TerminalPage::_WorkspacePinRequested(const TerminalApp::WorkspaceSidebar&, const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (item)
        {
            _ToggleWorkspacePin(item.SourceTab());
        }
    }

    void TerminalPage::_WorkspaceCloseRequested(const TerminalApp::WorkspaceSidebar&, const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (item)
        {
            if (const auto tab = item.SourceTab())
            {
                _HandleCloseTabRequested(tab);
            }
        }
    }

    void TerminalPage::_WorkspaceCloseOthersRequested(const TerminalApp::WorkspaceSidebar&, const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (item)
        {
            if (const auto keepTab = item.SourceTab())
            {
                std::vector<TerminalApp::Tab> tabsToClose;
                for (const auto& tab : _tabs)
                {
                    if (tab != keepTab)
                    {
                        tabsToClose.emplace_back(tab);
                    }
                }
                _RemoveTabs(std::move(tabsToClose));
            }
        }
    }

    void TerminalPage::_WorkspaceRenameRequested(const TerminalApp::WorkspaceSidebar&, const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (item)
        {
            if (const auto tab = item.SourceTab())
            {
                _RenameWorkspace(tab);
            }
        }
    }


    safe_void_coroutine TerminalPage::_WorkspaceConfigureAgentResumeRequested(const TerminalApp::WorkspaceSidebar&,
                                                                              const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item)
        {
            co_return;
        }

        const auto weakThis = get_weak();
        ContentDialog dialog{};
        dialog.Title(box_value(RS_(L"WorkspaceSidebar_AgentConfigureDialogTitle")));
        dialog.PrimaryButtonText(RS_(L"WorkspaceSidebar_AgentConfigureDialogSave"));
        dialog.CloseButtonText(RS_(L"WorkspaceSidebar_AgentDialogCancel"));
        dialog.DefaultButton(ContentDialogButton::Primary);

        TextBox textBox{};
        textBox.Header(box_value(RS_(L"WorkspaceSidebar_AgentConfigureDialogHeader")));
        textBox.PlaceholderText(RS_(L"WorkspaceSidebar_AgentConfigureDialogPlaceholder"));
        textBox.Text(item.AgentResumeSessionId());
        Automation::AutomationProperties::SetName(textBox, RS_(L"WorkspaceSidebar_AgentConfigureDialogHeader"));
        dialog.Content(textBox);

        if (const auto presenter = _dialogPresenter.get())
        {
            if (co_await presenter.ShowDialog(dialog) != ContentDialogResult::Primary)
            {
                co_return;
            }

            if (const auto page = weakThis.get())
            {
                const auto sessionId = WorkspaceAgentResume::NormalizeCodexSessionId(textBox.Text());
                if (sessionId.empty())
                {
                    ContentDialog errorDialog{};
                    errorDialog.Title(box_value(RS_(L"WorkspaceSidebar_AgentInvalidSessionDialogTitle")));
                    errorDialog.Content(box_value(RS_(L"WorkspaceSidebar_AgentInvalidSessionDialogBody")));
                    errorDialog.CloseButtonText(RS_(L"Ok"));
                    co_await presenter.ShowDialog(errorDialog);
                    co_return;
                }

                auto workingDirectory = item.WorkingDirectory();
                if (!Utils::IsValidDirectory(workingDirectory.c_str()))
                {
                    workingDirectory.clear();
                }
                item.ConfigureAgentResume(WorkspaceAgentResume::CodexProvider, sessionId, workingDirectory);
            }
        }
    }

    safe_void_coroutine TerminalPage::_WorkspaceResumeAgentRequested(const TerminalApp::WorkspaceSidebar&,
                                                                     const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item)
        {
            co_return;
        }

        AgentResumeInfo info{};
        info.Version = WorkspaceAgentResume::CurrentVersion;
        info.WorkspaceId = item.WorkspaceId();
        info.Provider = item.AgentResumeProvider();
        info.SessionId = item.AgentResumeSessionId();
        info.WorkingDirectory = item.AgentResumeWorkingDirectory();
        info.RequiresConfirmation = true;
        if (!WorkspaceAgentResume::IsSupported(info))
        {
            co_return;
        }

        _workspaceFeatures->History().ResumeSession(
            info.WorkspaceId,
            info.SessionId,
            info.WorkingDirectory.empty() ? item.WorkingDirectory() : info.WorkingDirectory);
        co_return;
    }


    void TerminalPage::_WorkspaceClearAgentResumeRequested(const TerminalApp::WorkspaceSidebar&,
                                                           const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (item)
        {
            item.ClearAgentResume();
        }
    }

    void TerminalPage::_WorkspaceActivateNotificationRequested(const TerminalApp::WorkspaceSidebar&, const TerminalApp::WorkspaceItemViewModel& item)
    {
        _FocusWorkspaceNotification(item);
    }

    void TerminalPage::_WorkspaceActivateNotificationItemRequested(const TerminalApp::WorkspaceSidebar&, const TerminalApp::WorkspaceNotification& notification)
    {
        if (const auto item = _WorkspaceItemForNotification(notification))
        {
            _FocusWorkspaceNotification(item, notification);
        }
    }

    void TerminalPage::_WorkspaceMarkNotificationsReadRequested(const TerminalApp::WorkspaceSidebar&, const TerminalApp::WorkspaceItemViewModel& item)
    {
        _MarkWorkspaceNotificationsRead(item);
    }

    void TerminalPage::_WorkspaceClearNotificationHistoryRequested(const TerminalApp::WorkspaceSidebar&, const TerminalApp::WorkspaceItemViewModel& item)
    {
        _ClearWorkspaceNotificationHistory(item);
    }

    void TerminalPage::_WorkspaceMarkAllNotificationsReadRequested(const TerminalApp::WorkspaceSidebar&, const IInspectable&)
    {
        _MarkAllWorkspaceNotificationsRead();
    }

    void TerminalPage::_WorkspaceClearAllNotificationHistoryRequested(const TerminalApp::WorkspaceSidebar&, const IInspectable&)
    {
        _ClearAllWorkspaceNotificationHistory();
    }

    TerminalApp::WorkspaceItemViewModel TerminalPage::_WorkspaceItemForTab(const TerminalApp::Tab& tab) const
    {
        for (const auto& item : _WorkspaceItems())
        {
            if (item.SourceTab() == tab)
            {
                return item;
            }
        }
        return nullptr;
    }

    winrt::guid TerminalPage::_ReserveWorkspaceIdForNewTab() const
    {
        const auto isAvailable = [&](const winrt::guid& candidate) {
            return candidate != winrt::guid{} &&
                   std::ranges::none_of(_WorkspaceItems(), [&](const auto& item) {
                       return item.WorkspaceId() == candidate;
                   });
        };

        // Workspace restoration is positional. Peek at the ID here, before the
        // Pane creates its ConPTY connection; _InitializeTab consumes the same
        // position only after the Pane was successfully created.
        if (_SessionRestore().Restoring && _SessionRestore().RestoreIndex < _SessionRestore().StartupWorkspaceIds.size())
        {
            const auto restoredId = _SessionRestore().StartupWorkspaceIds.at(_SessionRestore().RestoreIndex);
            if (isAvailable(restoredId))
            {
                return restoredId;
            }
        }

        for (;;)
        {
            GUID generated{};
            THROW_IF_FAILED(CoCreateGuid(&generated));
            if (isAvailable(generated))
            {
                return generated;
            }
        }
    }

    winrt::guid TerminalPage::_WorkspaceIdForTab(const TerminalApp::Tab& tab) const
    {
        if (const auto item = _WorkspaceItemForTab(tab))
        {
            return item.WorkspaceId();
        }
        return {};
    }

    winrt::guid TerminalPage::_WorkspaceIdForPaneContent(const TerminalApp::TerminalPaneContent& paneContent) const
    {
        if (!paneContent)
        {
            return {};
        }

        for (const auto& item : _WorkspaceItems())
        {
            if (const auto tab = _GetTabImpl(item.SourceTab()); tab && tab->GetRootPane())
            {
                const auto matchingPane = tab->GetRootPane()->WalkTree([&](const auto& pane) -> std::shared_ptr<Pane> {
                    const auto terminalContent = pane->GetContent().try_as<TerminalApp::TerminalPaneContent>();
                    return terminalContent == paneContent ? pane : nullptr;
                });
                if (matchingPane)
                {
                    return item.WorkspaceId();
                }
            }
        }
        return {};
    }

    TerminalApp::WorkspaceItemViewModel TerminalPage::_WorkspaceItemForNotification(const TerminalApp::WorkspaceNotification& notification) const
    {
        if (notification)
        {
            for (const auto& item : _WorkspaceItems())
            {
                if (item.WorkspaceId() == notification.WorkspaceId())
                {
                    return item;
                }
            }
        }
        return nullptr;
    }

    bool TerminalPage::_RecordWorkspaceNotification(const winrt::com_ptr<Tab>& tab,
                                                    const TerminalApp::IPaneContent& content,
                                                    const winrt::hstring& title,
                                                    const winrt::hstring& message)
    {
        if (!tab || !content)
        {
            return false;
        }

        // The user is already looking at this pane, so its bell is not unread.
        if (_activated && tab == _GetFocusedTabImpl())
        {
            if (const auto activePane = tab->GetActivePane(); activePane && activePane->GetContent() == content)
            {
                return false;
            }
        }

        const auto sourcePane = tab->GetRootPane()->WalkTree([&](const auto& pane) -> std::shared_ptr<Pane> {
            return pane->GetContent() == content ? pane : nullptr;
        });
        const auto item = _WorkspaceItemForTab(*tab);
        if (!sourcePane || !item)
        {
            return false;
        }

        if (const auto paneId = sourcePane->Id())
        {
            sourcePane->SetAttention(true);
            item.AddNotification(paneId.value(), title, message);
            _RefreshWorkspaceNotifications();
            return true;
        }
        return false;
    }

    bool TerminalPage::_FocusWorkspaceNotification(const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item)
        {
            return false;
        }

        return _FocusWorkspaceNotification(item, item.LatestUnreadNotification());
    }

    bool TerminalPage::_FocusWorkspaceNotification(const TerminalApp::WorkspaceItemViewModel& item, const TerminalApp::WorkspaceNotification& notification)
    {
        if (!item || !notification)
        {
            return false;
        }

        const auto tab = item.SourceTab();
        const auto tabImpl = _GetTabImpl(tab);
        const auto tabIndex = _GetTabIndex(tab);
        if (!tabImpl || !tabIndex)
        {
            return false;
        }

        const auto paneId = notification.PaneId();
        _SelectTab(tabIndex.value());
        if (tabImpl->FocusPane(paneId))
        {
            if (const auto pane = tabImpl->GetRootPane()->FindPane(paneId))
            {
                pane->SetAttention(false);
            }
            item.MarkNotificationsReadForPane(paneId);
            _RefreshWorkspaceNotifications();
            return true;
        }

        // The source pane may have been closed while its notification was unread.
        item.MarkNotificationsReadForPane(paneId);
        _RefreshWorkspaceNotifications();
        return false;
    }

    bool TerminalPage::_FocusLatestWorkspaceNotification()
    {
        TerminalApp::WorkspaceItemViewModel latestItem{ nullptr };
        int64_t latestTimestamp = std::numeric_limits<int64_t>::min();

        for (const auto& item : _WorkspaceItems())
        {
            if (const auto notification = item.LatestUnreadNotification())
            {
                if (const auto timestamp = notification.Timestamp().time_since_epoch().count(); !latestItem || timestamp > latestTimestamp)
                {
                    latestItem = item;
                    latestTimestamp = timestamp;
                }
            }
        }

        return _FocusWorkspaceNotification(latestItem);
    }

    void TerminalPage::_MarkWorkspaceNotificationsRead(const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item)
        {
            return;
        }

        item.MarkAllNotificationsRead();
        if (const auto tab = _GetTabImpl(item.SourceTab()))
        {
            tab->GetRootPane()->WalkTree([](const auto& pane) {
                pane->SetAttention(false);
            });
        }
        _RefreshWorkspaceNotifications();
    }

    void TerminalPage::_ClearWorkspaceNotificationHistory(const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item)
        {
            return;
        }

        item.ClearNotificationHistory();
        if (const auto tab = _GetTabImpl(item.SourceTab()))
        {
            tab->GetRootPane()->WalkTree([](const auto& pane) {
                pane->SetAttention(false);
            });
        }
        _RefreshWorkspaceNotifications();
    }

    void TerminalPage::_MarkAllWorkspaceNotificationsRead()
    {
        for (const auto& item : _WorkspaceItems())
        {
            item.MarkAllNotificationsRead();
            if (const auto tab = _GetTabImpl(item.SourceTab()))
            {
                tab->GetRootPane()->WalkTree([](const auto& pane) {
                    pane->SetAttention(false);
                });
            }
        }
        _RefreshWorkspaceNotifications();
    }

    void TerminalPage::_ClearAllWorkspaceNotificationHistory()
    {
        for (const auto& item : _WorkspaceItems())
        {
            item.ClearNotificationHistory();
            if (const auto tab = _GetTabImpl(item.SourceTab()))
            {
                tab->GetRootPane()->WalkTree([](const auto& pane) {
                    pane->SetAttention(false);
                });
            }
        }
        _RefreshWorkspaceNotifications();
    }

    void TerminalPage::_MarkWorkspaceNotificationsReadForActivePane(const winrt::com_ptr<Tab>& tab)
    {
        if (!tab)
        {
            return;
        }

        const auto pane = tab->GetActivePane();
        const auto item = _WorkspaceItemForTab(*tab);
        if (pane && item)
        {
            if (const auto paneId = pane->Id())
            {
                pane->SetAttention(false);
                item.MarkNotificationsReadForPane(paneId.value());
                _RefreshWorkspaceNotifications();
            }
        }
    }

    void TerminalPage::_RefreshWorkspaceNotifications()
    {
        if (_workspaceSidebar)
        {
            _workspaceSidebar.RefreshNotifications();
        }

        if (_tabRow && _workspaceNotificationButton)
        {
            uint32_t unreadCount = 0;
            for (const auto& item : _WorkspaceItems())
            {
                unreadCount += item.UnreadCount();
            }

            const auto tabRow = winrt::get_self<implementation::TabRowControl>(_tabRow);
            tabRow->WorkspaceNotificationBadgeText().Text(unreadCount == 0 ? winrt::hstring{} : winrt::to_hstring(unreadCount));
            tabRow->WorkspaceNotificationBadge().Visibility(unreadCount == 0 ? Visibility::Collapsed : Visibility::Visible);

            const auto automationName = unreadCount == 0 ?
                                            RS_(L"WorkspaceSidebar_NotificationCenterAutomationName") :
                                            winrt::hstring{ RS_fmt(L"WorkspaceSidebar_NotificationCenterUnreadAutomationName", unreadCount) };
            Automation::AutomationProperties::SetName(_workspaceNotificationButton, automationName);
        }
    }

    safe_void_coroutine TerminalPage::_RenameWorkspace(winrt::TerminalApp::Tab tab)
    {
        const auto weakThis = get_weak();
        ContentDialog dialog{};
        dialog.Title(box_value(RS_(L"WorkspaceSidebar_RenameDialogTitle")));
        dialog.PrimaryButtonText(RS_(L"WorkspaceSidebar_RenameDialogSave"));
        dialog.CloseButtonText(RS_(L"WorkspaceSidebar_RenameDialogCancel"));
        dialog.DefaultButton(ContentDialogButton::Primary);

        TextBox textBox{};
        textBox.Text(tab.Title());
        textBox.PlaceholderText(RS_(L"WorkspaceSidebar_RenameDialogPlaceholder"));
        Automation::AutomationProperties::SetName(textBox, RS_(L"WorkspaceSidebar_RenameDialogPlaceholder"));
        textBox.Loaded([weakTextBox{ winrt::make_weak(textBox) }](const auto&, const auto&) {
            if (const auto box = weakTextBox.get())
            {
                box.SelectAll();
                box.Focus(FocusState::Programmatic);
            }
        });
        dialog.Content(textBox);

        if (const auto presenter = _dialogPresenter.get())
        {
            const auto result = co_await presenter.ShowDialog(dialog);
            if (result == ContentDialogResult::Primary)
            {
                if (const auto page = weakThis.get())
                {
                    uint32_t index{};
                    if (page->_tabs.IndexOf(tab, index))
                    {
                        if (const auto tabImpl = _GetTabImpl(tab))
                        {
                            if (textBox.Text().empty())
                            {
                                tabImpl->ResetTabText();
                            }
                            else
                            {
                                tabImpl->SetTabText(textBox.Text());
                            }
                        }
                    }
                }
            }
        }
    }

    void TerminalPage::_WorkspaceList_SelectionChanged(const IInspectable& sender, const SelectionChangedEventArgs&)
    {
        if (_SidebarController().SyncingSelection || _removing || _SidebarController().Reordering)
        {
            return;
        }

        if (const auto list = sender.try_as<ListView>())
        {
            if (const auto item = list.SelectedItem().try_as<TerminalApp::WorkspaceItemViewModel>())
            {
                if (const auto tab = item.SourceTab())
                {
                    _tabView.SelectedItem(tab.TabViewItem());
                }
            }
        }
    }

    void TerminalPage::_WorkspaceList_DragItemsStarting(const IInspectable&, const DragItemsStartingEventArgs& args)
    {
        _SidebarController().ReorderFrom = std::nullopt;
        _SidebarController().ReorderItem = nullptr;

        if (args.Items().Size() == 1)
        {
            if (const auto item = args.Items().GetAt(0).try_as<TerminalApp::WorkspaceItemViewModel>())
            {
                if (const auto tab = item.SourceTab())
                {
                    uint32_t index{};
                    if (_tabs.IndexOf(tab, index))
                    {
                        _SidebarController().ReorderFrom = index;
                        _SidebarController().ReorderItem = item;
                    }
                }
            }
        }
    }

    void TerminalPage::_WorkspaceList_DragItemsCompleted(const IInspectable&, const DragItemsCompletedEventArgs&)
    {
        if (_SidebarController().ReorderFrom.has_value() && _SidebarController().ReorderItem)
        {
            uint32_t newIndex{};
            if (_WorkspaceItems().IndexOf(_SidebarController().ReorderItem, newIndex))
            {
                _SidebarController().Reordering = true;
                _TryMoveTab(_SidebarController().ReorderFrom.value(), gsl::narrow_cast<int32_t>(newIndex));

                // ListView has already reordered its ItemsSource. If the move
                // crossed the pinned boundary, repair the sidebar order to
                // match the clamped canonical tab order.
                uint32_t desiredIndex{};
                uint32_t actualIndex{};
                if (_tabs.IndexOf(_SidebarController().ReorderItem.SourceTab(), desiredIndex) &&
                    _WorkspaceItems().IndexOf(_SidebarController().ReorderItem, actualIndex) &&
                    desiredIndex != actualIndex)
                {
                    _workspaceFeatures->Workspaces().Move(actualIndex, desiredIndex);
                }
                _SidebarController().Reordering = false;
            }
        }

        _SidebarController().ReorderFrom = std::nullopt;
        _SidebarController().ReorderItem = nullptr;
    }

}
