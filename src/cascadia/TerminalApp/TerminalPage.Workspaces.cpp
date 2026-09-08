// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"
#include "Tasks/TaskPage.h"
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
        _SetWorkspaceToolPage(enabled ? _activeWorkspaceToolPage : WorkspaceToolPage::None);
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
            for (const auto& surface : item.Surfaces())
            {
                if (const auto tab = _GetTabImpl(surface); tab && tab->GetRootPane())
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
    }

    void TerminalPage::_Root_SizeChanged(const IInspectable&, const SizeChangedEventArgs& args)
    {
        _UpdateWorkspaceSidebarForWindowWidth(args.NewSize().Width);
    }

    void TerminalPage::_SyncWorkspaceSelection(const winrt::TerminalApp::Tab& tab)
    {
        if (!_workspaceList || !_workspaceSurfaceTabs || !tab)
        {
            return;
        }

        auto& workspaces = _workspaceFeatures->Workspaces();
        auto topology = workspaces.BeginUpdate();
        const auto workspaceIndex = workspaces.IndexOfTab(tab);
        if (!workspaceIndex)
        {
            return;
        }

        workspaces.SetActiveTab(tab);

        for (uint32_t index = 0; index < _WorkspaceItems().Size(); ++index)
        {
            _WorkspaceItems().GetAt(index).IsSelected(index == workspaceIndex.value());
        }

        const auto selectedIndex = gsl::narrow_cast<int32_t>(workspaceIndex.value());
        const auto wasSyncingSelection = std::exchange(_SidebarController().SyncingSelection, true);
        auto restoreSyncingSelection = wil::scope_exit([&]() noexcept { _SidebarController().SyncingSelection = wasSyncingSelection; });
        if (_workspaceList.SelectedIndex() != selectedIndex)
        {
            _workspaceList.SelectedIndex(selectedIndex);
        }

        const auto item = _WorkspaceItems().GetAt(workspaceIndex.value());
        if (_workspaceSurfaceTabs.ItemsSource().try_as<Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab>>() != item.Surfaces())
        {
            _workspaceSurfaceTabs.ItemsSource(item.Surfaces());
        }
        if (_workspaceSurfaceTabs.SelectedItem() != tab)
        {
            _workspaceSurfaceTabs.SelectedItem(tab);
        }
        if (const auto container = _workspaceList.ContainerFromItem(item).try_as<FrameworkElement>())
        {
            container.StartBringIntoView();
        }
        _SyncActiveProjectFromTab(tab);
    }

    void TerminalPage::_SetWorkspaceToolPage(const WorkspaceToolPage requestedPage)
    {
        if (!_tabContent || !_taskPage || !_conclusionSidebar)
        {
            return;
        }

        const auto page = _SidebarController().Enabled ? requestedPage : WorkspaceToolPage::None;
        _activeWorkspaceToolPage = page;
        const auto wasSyncingSelection = std::exchange(_SidebarController().SyncingSelection, true);
        auto restoreSyncingSelection = wil::scope_exit([&]() noexcept { _SidebarController().SyncingSelection = wasSyncingSelection; });

        if (_workspaceToolTabs)
        {
            switch (page)
            {
            case WorkspaceToolPage::Tasks:
                _workspaceToolTabs.SelectedItem(_workspaceTasksToolTab);
                break;
            case WorkspaceToolPage::Summary:
                _workspaceToolTabs.SelectedItem(_workspaceSummaryToolTab);
                break;
            default:
                _workspaceToolTabs.SelectedIndex(-1);
                break;
            }
        }
        if (page != WorkspaceToolPage::None && _workspaceSurfaceTabs)
        {
            _workspaceSurfaceTabs.SelectedIndex(-1);
        }
        else if (_workspaceSurfaceTabs)
        {
            if (const auto tab = _GetFocusedTab())
            {
                _workspaceSurfaceTabs.SelectedItem(tab);
            }
        }

        _tabContent.Visibility(page == WorkspaceToolPage::None ? Visibility::Visible : Visibility::Collapsed);
        TaskPanelHost().Visibility(page == WorkspaceToolPage::Tasks ? Visibility::Visible : Visibility::Collapsed);
        _taskPage.Visibility(page == WorkspaceToolPage::Tasks ? Visibility::Visible : Visibility::Collapsed);
        _conclusionSidebar.Visibility(page == WorkspaceToolPage::Summary ? Visibility::Visible : Visibility::Collapsed);

        if (page != WorkspaceToolPage::None)
        {
            for (const auto& tab : _tabs)
            {
                tab.Focus(FocusState::Unfocused);
            }
        }

        if (page == WorkspaceToolPage::Tasks)
        {
            const auto taskPage = winrt::get_self<implementation::TaskPage>(_taskPage);
            if (const auto workspace = _WorkspaceItemForTab(_GetFocusedTab()); !_SyncProjectTaskContext() && workspace)
            {
                taskPage->SetCurrentWorkspace(workspace.WorkspaceId(), workspace.GitRepositoryRoot(), workspace.WorkingDirectory());
            }
            taskPage->Refresh();
            if (_workspaceTasksToolTab)
            {
                _workspaceTasksToolTab.Focus(FocusState::Programmatic);
            }
        }
        else if (page == WorkspaceToolPage::Summary)
        {
            _workspaceFeatures->History().UpdateView();
            if (_workspaceSummaryToolTab)
            {
                _workspaceSummaryToolTab.Focus(FocusState::Programmatic);
            }
        }
        _RenderProjectNavigation();
    }

    void TerminalPage::_ApplyRestoredWorkspaceActiveSurfaces()
    {
        auto& workspaces = _workspaceFeatures->Workspaces();
        auto topology = workspaces.BeginUpdate();
        for (const auto& [workspaceId, surfaceId] : _SessionRestore().ActiveSurfaceIds)
        {
            if (const auto workspaceIndex = workspaces.IndexOf(workspaceId))
            {
                const auto item = _WorkspaceItems().GetAt(workspaceIndex.value());
                const auto itemImpl = winrt::get_self<WorkspaceItemViewModel>(item);
                if (const auto tab = itemImpl->TabForSurfaceId(surfaceId))
                {
                    itemImpl->SetActiveTab(tab);
                }
            }
        }

        // The persisted SwitchToTab action is authoritative for the selected
        // Workspace, even when partial/corrupt metadata had to be ignored.
        if (const auto focusedTab = _GetFocusedTab())
        {
            if (workspaces.ItemForTab(focusedTab))
            {
                workspaces.SetActiveTab(focusedTab);
            }
        }
    }

    bool TerminalPage::_SelectWorkspace(const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item)
        {
            return false;
        }

        if (const auto tabIndex = _workspaceFeatures->Workspaces().ActiveFlatTabIndex(item.WorkspaceId()))
        {
            return _SelectTab(tabIndex.value());
        }
        return false;
    }

    void TerminalPage::_SynchronizeFlatTabOrder()
    {
        auto topology = _workspaceFeatures->Workspaces().BeginUpdate();
        std::vector<TerminalApp::Tab> desiredOrder;
        desiredOrder.reserve(_tabs.Size());
        for (const auto& workspace : _WorkspaceItems())
        {
            for (const auto& surface : workspace.Surfaces())
            {
                desiredOrder.emplace_back(surface);
            }
        }
        THROW_HR_IF(E_UNEXPECTED, desiredOrder.size() != _tabs.Size());

        const auto selectedItem = _tabView.SelectedItem();
        const auto wasRemoving = std::exchange(_removing, true);
        auto restoreRemoving = wil::scope_exit([&]() noexcept { _removing = wasRemoving; });

        for (uint32_t targetIndex = 0; targetIndex < desiredOrder.size(); ++targetIndex)
        {
            if (_tabs.GetAt(targetIndex) == desiredOrder[targetIndex])
            {
                continue;
            }

            uint32_t sourceIndex{};
            THROW_HR_IF(E_UNEXPECTED, !_tabs.IndexOf(desiredOrder[targetIndex], sourceIndex));
            const auto tab = _tabs.GetAt(sourceIndex);
            _tabs.RemoveAt(sourceIndex);
            _tabs.InsertAt(targetIndex, tab);
        }

        for (uint32_t targetIndex = 0; targetIndex < desiredOrder.size(); ++targetIndex)
        {
            const auto tabViewItem = desiredOrder[targetIndex].TabViewItem();
            if (_tabView.TabItems().GetAt(targetIndex) == tabViewItem)
            {
                continue;
            }

            uint32_t visualSourceIndex{};
            THROW_HR_IF(E_UNEXPECTED, !_tabView.TabItems().IndexOf(tabViewItem, visualSourceIndex));
            _tabView.TabItems().RemoveAt(visualSourceIndex);
            _tabView.TabItems().InsertAt(targetIndex, tabViewItem);
        }

        _UpdateTabIndices();
        if (selectedItem)
        {
            _tabView.SelectedItem(selectedItem);
        }
        _workspaceFeatures->Workspaces().AssertInvariants(_tabs);
    }

    std::optional<uint32_t> TerminalPage::_FindWorkspaceIndex(const winrt::guid& workspaceId) const
    {
        return _workspaceFeatures->Workspaces().IndexOf(workspaceId);
    }

    void TerminalPage::_RecordWorkspaceNavigation(const winrt::TerminalApp::Tab& tab)
    {
        const auto item = _WorkspaceItemForTab(tab);
        if (!item)
        {
            return;
        }

        const auto workspaceId = item.WorkspaceId();
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
            return _SelectWorkspace(_WorkspaceItems().GetAt(_FindWorkspaceIndex(target).value()));
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
        const auto item = _WorkspaceItemForTab(tab);
        if (!item)
        {
            return;
        }

        auto& workspaces = _workspaceFeatures->Workspaces();
        auto topology = workspaces.BeginUpdate();
        const auto currentIndex = workspaces.IndexOf(item.WorkspaceId());
        if (!currentIndex)
        {
            return;
        }

        item.IsPinned(!item.IsPinned());
        uint32_t pinnedWorkspaceCount{};
        for (const auto& workspace : _WorkspaceItems())
        {
            pinnedWorkspaceCount += workspace.IsPinned() ? 1u : 0u;
        }
        const auto targetIndex = item.IsPinned() ? pinnedWorkspaceCount - 1 : pinnedWorkspaceCount;
        workspaces.Move(currentIndex.value(), targetIndex);
        _SynchronizeFlatTabOrder();
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
            const auto retainsTaskSurface = std::ranges::any_of(item.Surfaces(), [&](const auto& tab) {
                return _IsTaskOwnedTab(tab);
            });
            if (item.SurfaceCount() == 1)
            {
                _HandleCloseTabRequested(item.SourceTab());
                if (retainsTaskSurface) _SetWorkspaceToolPage(WorkspaceToolPage::Tasks);
                return;
            }
            std::vector<TerminalApp::Tab> tabsToClose;
            tabsToClose.reserve(item.SurfaceCount());
            for (const auto& tab : item.Surfaces())
            {
                tabsToClose.emplace_back(tab);
            }
            _RemoveTabs(std::move(tabsToClose));
            if (retainsTaskSurface) _SetWorkspaceToolPage(WorkspaceToolPage::Tasks);
        }
    }

    void TerminalPage::_WorkspaceCloseOthersRequested(const TerminalApp::WorkspaceSidebar&, const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (item)
        {
            std::vector<TerminalApp::Tab> tabsToClose;
            bool retainsTaskSurface{};
            for (const auto& tab : _tabs)
            {
                const auto belongsToWorkspace = std::ranges::any_of(item.Surfaces(), [&](const auto& surface) {
                    return surface == tab;
                });
                if (!belongsToWorkspace)
                {
                    retainsTaskSurface = retainsTaskSurface || _IsTaskOwnedTab(tab);
                    tabsToClose.emplace_back(tab);
                }
            }
            _RemoveTabs(std::move(tabsToClose));
            if (retainsTaskSurface) _SetWorkspaceToolPage(WorkspaceToolPage::Tasks);
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

    void TerminalPage::_WorkspaceSurfaceTabs_SelectionChanged(const IInspectable& sender, const SelectionChangedEventArgs&)
    {
        if (_SidebarController().SyncingSelection || _removing || _rearranging)
        {
            return;
        }

        if (const auto list = sender.try_as<ListView>())
        {
            if (const auto tab = list.SelectedItem().try_as<TerminalApp::Tab>())
            {
                const auto restoreFocus = _activeWorkspaceToolPage != WorkspaceToolPage::None && tab == _GetFocusedTab();
                _SetWorkspaceToolPage(WorkspaceToolPage::None);
                if (const auto tabIndex = _GetTabIndex(tab))
                {
                    _SelectTab(tabIndex.value());
                    if (restoreFocus)
                    {
                        tab.Focus(FocusState::Programmatic);
                    }
                }
            }
        }
    }

    void TerminalPage::_WorkspaceSurfaceTabClose_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto element = sender.try_as<FrameworkElement>())
        {
            if (const auto tab = element.Tag().try_as<TerminalApp::Tab>())
            {
                _HandleCloseTabRequested(tab);
            }
        }
    }

    void TerminalPage::_WorkspaceSurfaceNewTab_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (const auto item = _WorkspaceItemForTab(_GetFocusedTab()))
        {
            _OpenNewWorkspaceSurface(item);
        }
    }

    void TerminalPage::_WorkspaceToolTabs_SelectionChanged(const IInspectable& sender, const SelectionChangedEventArgs&)
    {
        if (_SidebarController().SyncingSelection || _removing || _rearranging)
        {
            return;
        }

        if (const auto list = sender.try_as<ListView>())
        {
            const auto selected = list.SelectedItem();
            if (selected == _workspaceTasksToolTab)
            {
                _SetWorkspaceToolPage(WorkspaceToolPage::Tasks);
            }
            else if (selected == _workspaceSummaryToolTab)
            {
                _SetWorkspaceToolPage(WorkspaceToolPage::Summary);
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
        return _workspaceFeatures->Workspaces().ItemForTab(tab);
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
            if ((_SessionRestore().StateVersion == CurrentWorkspaceStateVersion && restoredId != winrt::guid{}) || isAvailable(restoredId))
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

    winrt::guid TerminalPage::_WorkspaceIdForNewTab() const
    {
        // Startup actions retain their historical one-Tab-per-Workspace
        // semantics. Once the window is live, New Tab in Sidebar mode creates
        // a Surface inside the active Workspace.
        if (_SidebarController().Enabled && _startupState == StartupState::Initialized)
        {
            if (const auto activeTab = _GetFocusedTab())
            {
                if (const auto workspaceId = _WorkspaceIdForTab(activeTab); workspaceId != winrt::guid{})
                {
                    return workspaceId;
                }
            }
        }
        return _ReserveWorkspaceIdForNewTab();
    }

    void TerminalPage::_OpenNewWorkspace()
    {
        const auto workspaceId = _ReserveWorkspaceIdForNewTab();
        if (const auto pane = _MakePane(NewTerminalArgs(), nullptr, nullptr, true, workspaceId))
        {
            _CreateNewTabFromPane(pane, -1, workspaceId);
        }
    }

    void TerminalPage::_OpenNewWorkspaceSurface(const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item || !_FindWorkspaceIndex(item.WorkspaceId()))
        {
            return;
        }

        if (const auto pane = _MakePane(NewTerminalArgs(), nullptr, nullptr, true, item.WorkspaceId()))
        {
            _CreateNewTabFromPane(pane, -1, item.WorkspaceId());
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
            for (const auto& surface : item.Surfaces())
            {
                if (const auto tab = _GetTabImpl(surface); tab && tab->GetRootPane())
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
            const auto surfaceId = winrt::get_self<WorkspaceItemViewModel>(item)->SurfaceIdForTab(*tab);
            item.AddNotification(surfaceId, paneId.value(), title, message);
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

        const auto itemImpl = winrt::get_self<WorkspaceItemViewModel>(item);
        const auto tab = itemImpl->TabForSurfaceId(notification.SurfaceId());
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
            item.MarkNotificationsReadForSurfacePane(notification.SurfaceId(), paneId);
            _RefreshWorkspaceNotifications();
            return true;
        }

        // The source pane may have been closed while its notification was unread.
        item.MarkNotificationsReadForSurfacePane(notification.SurfaceId(), paneId);
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
                const auto surfaceId = winrt::get_self<WorkspaceItemViewModel>(item)->SurfaceIdForTab(*tab);
                item.MarkNotificationsReadForSurfacePane(surfaceId, paneId.value());
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
                _SelectWorkspace(item);
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
                if (const auto index = _workspaceFeatures->Workspaces().IndexOf(item.WorkspaceId()))
                {
                    _SidebarController().ReorderFrom = index;
                    _SidebarController().ReorderItem = item;
                }
            }
        }
    }

    void TerminalPage::_WorkspaceList_DragItemsCompleted(const IInspectable&, const DragItemsCompletedEventArgs&)
    {
        auto topology = _workspaceFeatures->Workspaces().BeginUpdate();
        if (_SidebarController().ReorderFrom.has_value() && _SidebarController().ReorderItem)
        {
            uint32_t newIndex{};
            if (_WorkspaceItems().IndexOf(_SidebarController().ReorderItem, newIndex))
            {
                _SidebarController().Reordering = true;
                auto restoreReordering = wil::scope_exit([&]() noexcept { _SidebarController().Reordering = false; });

                uint32_t pinnedWorkspaceCount{};
                for (const auto& item : _WorkspaceItems())
                {
                    pinnedWorkspaceCount += item.IsPinned() ? 1u : 0u;
                }
                const auto maximumPinnedIndex = pinnedWorkspaceCount == 0 ? 0u : pinnedWorkspaceCount - 1;
                const auto clampedIndex = _SidebarController().ReorderItem.IsPinned() ?
                                              std::min(newIndex, maximumPinnedIndex) :
                                              std::max(newIndex, pinnedWorkspaceCount);
                if (newIndex != clampedIndex)
                {
                    _workspaceFeatures->Workspaces().Move(newIndex, clampedIndex);
                }
                _workspaceFeatures->Workspaces().NotifyNativeReorder();
                _SynchronizeFlatTabOrder();
            }
        }

        _SidebarController().ReorderFrom = std::nullopt;
        _SidebarController().ReorderItem = nullptr;
    }

    Windows::Foundation::Collections::IVectorView<TerminalApp::WorkspaceItemViewModel> TerminalPage::WorkspaceItemsView() const
    {
        return _WorkspaceItems().GetView();
    }

    TerminalApp::WorkspaceItemViewModel TerminalPage::ActiveWorkspace() const
    {
        return _workspaceFeatures->Workspaces().ItemForTab(_GetFocusedTab());
    }

    bool TerminalPage::WorkspaceUiReady() const noexcept
    {
        return _tabView && !_workspaceFeatures->Workspaces().IsUpdating();
    }

    bool TerminalPage::WorkspaceSidebarEnabled() const noexcept { return _SidebarController().Enabled; }

    AgentHistoryCoordinator& TerminalPage::WorkspaceHistory() noexcept { return _workspaceFeatures->History(); }

    AgentHistoryRetentionPolicy TerminalPage::WorkspaceHistoryRetention() const
    {
        const auto settings = _settings.GlobalSettings();
        AgentHistoryRetentionPolicy policy;
        policy.RetentionDays = std::min(settings.AgentHistoryRetentionDays(), uint32_t{ 36500 });
        policy.MaximumTurns = std::min(settings.AgentHistoryMaximumTurns(), uint32_t{ 1000000 });
        policy.MaximumPayloadBytes = static_cast<uint64_t>(std::min(settings.AgentHistoryMaximumPayloadMiB(), uint32_t{ 4096 })) * 1024 * 1024;
        policy.ReceiptTombstoneDays = 30;
        return policy;
    }

    void TerminalPage::OpenHistorySession(const winrt::guid& workspaceId, const NewTerminalArgs& args)
    {
        const auto index = _workspaceFeatures->Workspaces().IndexOf(workspaceId);
        if (!index) return;
        const auto tab = _WorkspaceItems().GetAt(*index).SourceTab();
        const auto tabIndex = tab ? _GetTabIndex(tab) : std::nullopt;
        if (!tabIndex) return;
        _SelectTab(*tabIndex);
        if (const auto pane = _MakePane(args, nullptr, nullptr, true, workspaceId))
            _SplitPane(_GetTabImpl(tab), SplitDirection::Automatic, 0.5f, pane, workspaceId);
    }

}
