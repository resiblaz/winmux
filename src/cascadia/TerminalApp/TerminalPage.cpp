
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"
#include "Tasks/RuntimeTaskProjectionSource.h"

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
#include "WorkspaceAutomation.h"
#include "WorkspaceFeatureSet.h"
#include "WorkspaceListeningPorts.h"

#include "LaunchPositionRequest.g.cpp"
#include "WindowListEntry.g.cpp"
#include "WindowListRequest.g.cpp"
#include "RenameWindowRequestedArgs.g.cpp"
#include "OpenWindowRequestedArgs.g.cpp"
#include "RequestMoveContentArgs.g.cpp"
#include "TerminalPage.g.cpp"

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
{
    TerminalPage::TerminalPage(TerminalApp::WindowProperties properties, const TerminalApp::ContentManager& manager) :
        _tabs{ winrt::single_threaded_observable_vector<TerminalApp::Tab>() },
        _mruTabs{ winrt::single_threaded_observable_vector<TerminalApp::Tab>() },
        _manager{ manager },
        _hostingHwnd{},
        _WindowProperties{ std::move(properties) }
    {
        InitializeComponent();
        _workspaceFeatures = std::make_unique<WorkspaceFeatureSet>(*this);
        _WindowProperties.PropertyChanged({ get_weak(), &TerminalPage::_windowPropertyChanged });
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel> TerminalPage::_WorkspaceItems() const noexcept
    {
        return _workspaceFeatures ? _workspaceFeatures->Workspaces().Items() : nullptr;
    }

    WorkspaceSessionRestoreState& TerminalPage::_SessionRestore() noexcept
    {
        return _workspaceFeatures->SessionRestore();
    }

    const WorkspaceSessionRestoreState& TerminalPage::_SessionRestore() const noexcept
    {
        return _workspaceFeatures->SessionRestore();
    }

    WorkspaceSidebarController& TerminalPage::_SidebarController() noexcept
    {
        return _workspaceFeatures->Sidebar();
    }

    const WorkspaceSidebarController& TerminalPage::_SidebarController() const noexcept
    {
        return _workspaceFeatures->Sidebar();
    }

    TerminalPage::~TerminalPage()
    {
        if (_workspaceFeatures)
        {
            _workspaceFeatures->Stop();
            _workspaceFeatures.reset();
        }
    }

    // Method Description:
    // - implements the IInitializeWithWindow interface from shobjidl_core.
    // - We're going to use this HWND as the owner for the ConPTY windows, via
    //   ConptyConnection::ReparentWindow. We need this for applications that
    //   call GetConsoleWindow, and attempt to open a MessageBox for the
    //   console. By marking the conpty windows as owned by the Terminal HWND,
    //   the message box will be owned by the Terminal window as well.
    //   - see GH#2988
    HRESULT TerminalPage::Initialize(HWND hwnd)
    {
        if (!_hostingHwnd.has_value())
        {
            // GH#13211 - if we haven't yet set the owning hwnd, reparent all the controls now.
            for (const auto& tab : _tabs)
            {
                if (auto tabImpl{ _GetTabImpl(tab) })
                {
                    tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                        if (const auto& term{ pane->GetTerminalControl() })
                        {
                            term.OwningHwnd(reinterpret_cast<uint64_t>(hwnd));
                        }
                    });
                }
                // We don't need to worry about resetting the owning hwnd for the
                // SUI here. GH#13211 only repros for a defterm connection, where
                // the tab is spawned before the window is created. It's not
                // possible to make a SUI tab like that, before the window is
                // created. The SUI could be spawned as a part of a window restore,
                // but that would still work fine. The window would be created
                // before restoring previous tabs in that scenario.
            }
        }

        _hostingHwnd = hwnd;
        return S_OK;
    }

    // INVARIANT: This needs to be called on OUR UI thread!
    void TerminalPage::SetSettings(CascadiaSettings settings, bool needRefreshUI)
    {
        assert(Dispatcher().HasThreadAccess());
        if (_settings == nullptr)
        {
            // Create this only on the first time we load the settings.
            _terminalSettingsCache = std::make_shared<TerminalSettingsCache>(settings, settings.WindowSettings(_WindowProperties.WindowName()));
        }
        _settings = settings;

        // Make sure to call SetCommands before _RefreshUIForSettingsReload.
        // SetCommands will make sure the KeyChordText of Commands is updated, which needs
        // to happen before the Settings UI is reloaded and tries to re-read those values.
        if (const auto p = CommandPaletteElement())
        {
            p.SetActionMap(_settings.ActionMap());
        }

        if (needRefreshUI)
        {
            _RefreshUIForSettingsReload();
        }

        // Upon settings update we reload the system settings for scrolling as well.
        // TODO: consider reloading this value periodically.
        _systemRowsToScroll = _ReadSystemRowsToScroll();
    }

    winrt::Microsoft::Terminal::Settings::Model::WindowSettings TerminalPage::_currentWindowSettings() const
    {
        return _settings.WindowSettings(_WindowProperties.WindowName());
    }

    bool TerminalPage::IsRunningElevated() const noexcept
    {
        // GH#2455 - Make sure to try/catch calls to Application::Current,
        // because that _won't_ be an instance of TerminalApp::App in the
        // LocalTests
        try
        {
            return Application::Current().as<TerminalApp::App>().Logic().IsRunningElevated();
        }
        CATCH_LOG();
        return false;
    }
    bool TerminalPage::CanDragDrop() const noexcept
    {
        try
        {
            return Application::Current().as<TerminalApp::App>().Logic().CanDragDrop();
        }
        CATCH_LOG();
        return true;
    }

    void TerminalPage::Create()
    {
        // Hookup the key bindings
        _HookupKeyBindings(_settings.ActionMap());

        _tabContent = this->TabContent();
        _tabRow = this->TabRow();
        _tabView = _tabRow.TabView();
        _workspaceSidebar = this->WorkspaceSidebar();
        _taskPage = this->TasksPage();
        _taskAgentSessions = std::make_shared<tasks::TaskAgentSessionRegistry>();
        _workspaceFeatures->History().SetTaskSessionRegistry(_taskAgentSessions);
        winrt::get_self<implementation::TaskPage>(_taskPage)->SetSurfaceHandler(
            [weakThis{ get_weak() }](const tasks::TaskSurfaceDescriptor& descriptor) {
                if (const auto page = weakThis.get())
                {
                    page->_HandleTaskSurface(descriptor);
                }
            });
        auto taskSource = tasks::MakeRuntimeTaskProjectionSource(_taskAgentSessions);
        std::array<wchar_t, 8> prototypeFlag{};
        if (!taskSource && GetEnvironmentVariableW(
                               L"RICHER_TASKS_USE_PROTOTYPE",
                               prototypeFlag.data(),
                               static_cast<DWORD>(prototypeFlag.size())) == 1 && prototypeFlag[0] == L'1')
        {
            taskSource = tasks::MakePrototypeTaskProjectionSource();
        }
        winrt::get_self<implementation::TaskPage>(_taskPage)->AttachProjectionSource(std::move(taskSource));
        _conclusionSidebar = this->ConclusionSidebar();
        winrt::get_self<implementation::ConclusionSidebar>(_conclusionSidebar)->SetTabContentMode(true);
        _workspaceFeatures->History().Attach(_conclusionSidebar);
        _workspaceList = _workspaceSidebar.WorkspaceList();
        _workspaceSurfaceTabBar = this->WorkspaceSurfaceTabBar();
        _workspaceSurfaceTabs = this->WorkspaceSurfaceTabs();
        _workspaceSurfaceNewTabButton = this->WorkspaceSurfaceNewTabButton();
        _workspaceToolTabs = this->WorkspaceToolTabs();
        _workspaceTasksToolTab = this->WorkspaceTasksToolTab();
        _workspaceSummaryToolTab = this->WorkspaceSummaryToolTab();
        _workspaceList.ItemsSource(_WorkspaceItems());
        _workspaceItemsChangedRevoker = _WorkspaceItems().VectorChanged(winrt::auto_revoke, [weakThis{ get_weak() }](auto&&, auto&&) {
            if (const auto page = weakThis.get())
            {
                page->_RefreshWorkspaceNotifications();
                page->_RefreshWorkspaceListeningPorts();
                page->_workspaceFeatures->History().OnWorkspaceItemsChanged();
            }
        });
        _RefreshWorkspaceNotifications();
        _workspaceFeatures->Snapshots().RefreshCache();
        _SidebarController().ListeningPortsTimer.Interval(5s);
        _SidebarController().ListeningPortsTimer.Tick([weakThis{ get_weak() }](const auto&, const auto&) {
            if (const auto page = weakThis.get())
            {
                if (!page->_SidebarController().ListeningPortsRefreshInFlight)
                {
                    page->_RefreshWorkspaceListeningPorts();
                }
            }
        });
        _rearranging = false;

        const auto canDragDrop = CanDragDrop();

        _tabView.CanReorderTabs(canDragDrop);
        _tabView.CanDragTabs(canDragDrop);
        _workspaceList.CanReorderItems(canDragDrop);
        _workspaceList.CanDragItems(canDragDrop);
        _tabView.TabDragStarting({ get_weak(), &TerminalPage::_TabDragStarted });
        _tabView.TabDragCompleted({ get_weak(), &TerminalPage::_TabDragCompleted });

        _workspaceSidebar.PinRequested({ get_weak(), &TerminalPage::_WorkspacePinRequested });
        _workspaceSidebar.CloseRequested({ get_weak(), &TerminalPage::_WorkspaceCloseRequested });
        _workspaceSidebar.CloseOthersRequested({ get_weak(), &TerminalPage::_WorkspaceCloseOthersRequested });
        _workspaceSidebar.RenameRequested({ get_weak(), &TerminalPage::_WorkspaceRenameRequested });
        _workspaceSidebar.SaveSnapshotRequested([weakThis{ get_weak() }](const auto&, const auto& item) {
            if (const auto page = weakThis.get(); page && page->_workspaceFeatures)
            {
                page->_workspaceFeatures->Snapshots().SaveRequested(item);
            }
        });
        _workspaceSidebar.ConfigureAgentResumeRequested({ get_weak(), &TerminalPage::_WorkspaceConfigureAgentResumeRequested });
        _workspaceSidebar.ResumeAgentRequested({ get_weak(), &TerminalPage::_WorkspaceResumeAgentRequested });
        _workspaceSidebar.ClearAgentResumeRequested({ get_weak(), &TerminalPage::_WorkspaceClearAgentResumeRequested });
        _workspaceSidebar.ActivateNotificationRequested({ get_weak(), &TerminalPage::_WorkspaceActivateNotificationRequested });
        _workspaceSidebar.ActivateNotificationItemRequested({ get_weak(), &TerminalPage::_WorkspaceActivateNotificationItemRequested });
        _workspaceSidebar.MarkNotificationsReadRequested({ get_weak(), &TerminalPage::_WorkspaceMarkNotificationsReadRequested });
        _workspaceSidebar.ClearNotificationHistoryRequested({ get_weak(), &TerminalPage::_WorkspaceClearNotificationHistoryRequested });
        _workspaceSidebar.MarkAllNotificationsReadRequested({ get_weak(), &TerminalPage::_WorkspaceMarkAllNotificationsReadRequested });
        _workspaceSidebar.ClearAllNotificationHistoryRequested({ get_weak(), &TerminalPage::_WorkspaceClearAllNotificationHistoryRequested });
        _workspaceList.SelectionChanged({ get_weak(), &TerminalPage::_WorkspaceList_SelectionChanged });
        _workspaceList.DragItemsStarting({ get_weak(), &TerminalPage::_WorkspaceList_DragItemsStarting });
        _workspaceList.DragItemsCompleted({ get_weak(), &TerminalPage::_WorkspaceList_DragItemsCompleted });

        auto tabRowImpl = winrt::get_self<implementation::TabRowControl>(_tabRow);
        _newTabButton = tabRowImpl->NewTabButton();
        _workspaceNewTabButton = tabRowImpl->WorkspaceNewTabButton();
        _workspaceNotificationButton = tabRowImpl->WorkspaceNotificationButton();
        _workspaceSidebarToggleButton = tabRowImpl->WorkspaceSidebarToggleButton();
        _workspaceFlyout = tabRowImpl->WorkspaceFlyout();
        _workspaceDropdown = tabRowImpl->WorkspaceDropdown();

        // Set the initial workspace name from the window name.
        // Use raw WindowName() so unnamed windows show no text.
        _tabRow.WorkspaceName(_WindowProperties.WindowName());

        // Rebuild the workspace flyout each time it opens so it always
        // reflects the latest set of persisted workspaces.
        _workspaceFlyout.Opening([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                page->_workspaceFeatures->Snapshots().PopulateFlyout();
            }
        });

        if (_currentWindowSettings().ShowTabsInTitlebar())
        {
            // GH#13143 Manually set the tab row's background to transparent here.
            //
            // We're doing it this way because ThemeResources are tricky. We
            // default in XAML to using the appropriate ThemeResource background
            // color for our TabRow. When tabs in the titlebar are _disabled_,
            // this will ensure that the tab row has the correct theme-dependent
            // value. When tabs in the titlebar are _enabled_ (the default),
            // we'll switch the BG to Transparent, to let the Titlebar Control's
            // background be used as the BG for the tab row.
            //
            // We can't do it the other way around (default to Transparent, only
            // switch to a color when disabling tabs in the titlebar), because
            // looking up the correct ThemeResource from and App dictionary is a
            // capital-H Hard problem.
            const auto transparent = Media::SolidColorBrush();
            transparent.Color(Windows::UI::Colors::Transparent());
            _tabRow.Background(transparent);
        }

        // This also moves the horizontal tab row into or out of the titlebar.
        // The TabView remains alive in sidebar mode as the source of truth for
        // selection and existing tab lifecycle behavior.
        _UpdateWorkspaceSidebar();
        _updateThemeColors();

        // Initialize the state of the CloseButtonOverlayMode property of
        // our TabView, to match the tab.showCloseButton property in the theme.
        if (const auto theme = _settings.GlobalSettings().CurrentTheme(_currentWindowSettings()))
        {
            const auto visibility = theme.Tab() ? theme.Tab().ShowCloseButton() : Settings::Model::TabCloseButtonVisibility::Always;

            _tabItemMiddleClickHookEnabled = visibility == Settings::Model::TabCloseButtonVisibility::Never;

            switch (visibility)
            {
            case Settings::Model::TabCloseButtonVisibility::Never:
                _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Auto);
                break;
            case Settings::Model::TabCloseButtonVisibility::Hover:
                _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::OnPointerOver);
                break;
            default:
                _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Always);
                break;
            }
        }

        // Hookup our event handlers to the ShortcutActionDispatch
        _RegisterActionCallbacks();

        //Event Bindings (Early)
        const auto openDefaultProfile = [weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuDefaultButtonClicked",
                    TraceLoggingDescription("Event emitted when the default button from the new tab split button is invoked"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                page->_OpenNewTerminalViaDropdown(NewTerminalArgs());
            }
        };
        _newTabButton.Click(openDefaultProfile);
        _workspaceNewTabButton.Click(openDefaultProfile);
        _workspaceNotificationButton.Click([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                winrt::get_self<implementation::WorkspaceSidebar>(page->_workspaceSidebar)->OpenNotificationCenter(page->_workspaceNotificationButton);
            }
        });
        _workspaceSidebarToggleButton.Click([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                page->_workspaceSidebar.IsExpanded(!page->_workspaceSidebar.IsExpanded());
                page->_UpdateWorkspaceTitleBar();
            }
        });
        _newTabButton.Drop({ get_weak(), &TerminalPage::_NewTerminalByDrop });
        _tabView.SelectionChanged({ this, &TerminalPage::_OnTabSelectionChanged });
        _tabView.TabCloseRequested({ this, &TerminalPage::_OnTabCloseRequested });
        _tabView.TabItemsChanged({ this, &TerminalPage::_OnTabItemsChanged });

        _tabView.TabDragStarting({ this, &TerminalPage::_onTabDragStarting });
        _tabView.TabStripDragOver({ this, &TerminalPage::_onTabStripDragOver });
        _tabView.TabStripDrop({ this, &TerminalPage::_onTabStripDrop });
        _tabView.TabDroppedOutside({ this, &TerminalPage::_onTabDroppedOutside });

        _CreateNewTabFlyout();
        _RefreshWorkspaceNotifications();

        _UpdateTabWidthMode();

        // Settings AllowDependentAnimations will affect whether animations are
        // enabled application-wide, so we don't need to check it each time we
        // want to create an animation.
        WUX::Media::Animation::Timeline::AllowDependentAnimations(!_currentWindowSettings().DisableAnimations());

        // Once the page is actually laid out on the screen, trigger all our
        // startup actions. Things like Panes need to know at least how big the
        // window will be, so they can subdivide that space.
        //
        // _OnFirstLayout will remove this handler so it doesn't get called more than once.
        _layoutUpdatedRevoker = _tabContent.LayoutUpdated(winrt::auto_revoke, { this, &TerminalPage::_OnFirstLayout });

        _isAlwaysOnTop = _currentWindowSettings().AlwaysOnTop();
        _showTabsFullscreen = _currentWindowSettings().ShowTabsFullscreen();

        // DON'T set up Toasts/TeachingTips here. They should be loaded and
        // initialized the first time they're opened, in whatever method opens
        // them.

        _tabRow.ShowElevationShield(IsRunningElevated() && _currentWindowSettings().ShowAdminShield());

        // Apply the ShowWorkspacesButton theme setting.
        if (const auto theme = _settings.GlobalSettings().CurrentTheme(_currentWindowSettings()))
        {
            _tabRow.ShowWorkspacesButton(theme.Window() ? theme.Window().ShowWorkspacesButton() : true);
        }

        _adjustProcessPriorityThrottled = std::make_shared<ThrottledFunc<>>(
            DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 100 },
                .debounce = true,
                .trailing = true,
            },
            [=]() {
                _adjustProcessPriority();
            });
    }


}
