// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <ThrottledFunc.h>

#include "TerminalPage.g.h"
#include "Tab.h"
#include "AppKeyBindings.h"
#include "AppCommandlineArgs.h"
#include "RenameWindowRequestedArgs.g.h"
#include "OpenWindowRequestedArgs.g.h"
#include "SummonWindowByIdRequestedArgs.g.h"
#include "RequestMoveContentArgs.g.h"
#include "LaunchPositionRequest.g.h"
#include "WindowListEntry.g.h"
#include "WindowListRequest.g.h"
#include "Toast.h"
#include "WorkspaceItemViewModel.h"
#include "WorkspaceSidebar.h"
#include "ConclusionSidebar.h"
#include "Tasks/TaskPage.h"
#include "Tasks/TaskSurfacePolicy.h"
#include "Tasks/TaskAgentSessionRegistry.h"
#include "AgentHistoryInbox.h"
#include "WorkspaceAgentResume.h"

#include "WindowsPackageManagerFactory.h"

#define DECLARE_ACTION_HANDLER(action) void _Handle##action(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);

namespace TerminalAppLocalTests
{
    class TabTests;
    class SettingsTests;
}

namespace Microsoft::Terminal::Core
{
    class ControlKeyStates;
}

namespace winrt::Microsoft::Terminal::Settings
{
    struct TerminalSettingsCreateResult;
}

namespace winrt::TerminalApp::implementation
{
    struct TerminalSettingsCache;
    class WorkspaceFeatureSet;
    class WorkspaceAutomationController;
    class AgentHistoryCoordinator;
    class WorkspaceSnapshotCoordinator;
    struct WorkspaceSessionRestoreState;
    class WorkspaceSidebarController;

    inline constexpr uint32_t DefaultRowsToScroll{ 3 };
    inline constexpr std::wstring_view TabletInputServiceKey{ L"TabletInputService" };

    enum StartupState : int
    {
        NotInitialized = 0,
        InStartup = 1,
        Initialized = 2
    };

    enum ScrollDirection : int
    {
        ScrollUp = 0,
        ScrollDown = 1
    };

    enum class ConfirmCloseDialogKind
    {
        Pane,
        Tab,
        MultiplePanes,
        MultipleTabs,
        Window,
        CloseAll
    };

    enum class WorkspaceToolPage : uint8_t
    {
        None,
        Tasks,
        Summary,
    };

    struct RenameWindowRequestedArgs : RenameWindowRequestedArgsT<RenameWindowRequestedArgs>
    {
        WINRT_PROPERTY(winrt::hstring, ProposedName);

    public:
        RenameWindowRequestedArgs(const winrt::hstring& name) :
            _ProposedName{ name } {};
    };

    struct OpenWindowRequestedArgs : OpenWindowRequestedArgsT<OpenWindowRequestedArgs>
    {
        WINRT_PROPERTY(winrt::hstring, Name);

    public:
        OpenWindowRequestedArgs(const winrt::hstring& name) :
            _Name{ name } {};
    };

    struct SummonWindowByIdRequestedArgs : SummonWindowByIdRequestedArgsT<SummonWindowByIdRequestedArgs>
    {
        WINRT_PROPERTY(uint64_t, WindowId);

    public:
        SummonWindowByIdRequestedArgs(uint64_t id) :
            _WindowId{ id } {};
    };

    struct RequestMoveContentArgs : RequestMoveContentArgsT<RequestMoveContentArgs>
    {
        WINRT_PROPERTY(winrt::hstring, Window);
        WINRT_PROPERTY(winrt::hstring, Content);
        WINRT_PROPERTY(uint32_t, TabIndex);
        WINRT_PROPERTY(Windows::Foundation::IReference<Windows::Foundation::Point>, WindowPosition);
        WINRT_PROPERTY(winrt::guid, WorkspaceId);
        WINRT_PROPERTY(winrt::guid, SourceSnapshotId);
        WINRT_PROPERTY(uint64_t, SourceSnapshotRevision, 0);

    public:
        RequestMoveContentArgs(const winrt::hstring window, const winrt::hstring content, uint32_t tabIndex) :
            _Window{ window },
            _Content{ content },
            _TabIndex{ tabIndex } {};
    };

    struct LaunchPositionRequest : LaunchPositionRequestT<LaunchPositionRequest>
    {
        LaunchPositionRequest() = default;

        til::property<winrt::Microsoft::Terminal::Settings::Model::LaunchPosition> Position;
    };

    struct WindowListEntry : WindowListEntryT<WindowListEntry>
    {
        WindowListEntry() = default;

        til::property<uint64_t> Id;
        til::property<winrt::hstring> Name;
    };

    struct WindowListRequest : WindowListRequestT<WindowListRequest>
    {
        WindowListRequest() :
            _Entries{ winrt::single_threaded_vector<winrt::TerminalApp::WindowListEntry>() } {}

        winrt::Windows::Foundation::Collections::IVector<winrt::TerminalApp::WindowListEntry> Entries() const { return _Entries; }

    private:
        winrt::Windows::Foundation::Collections::IVector<winrt::TerminalApp::WindowListEntry> _Entries;
    };

    struct WinGetSearchParams
    {
        winrt::Microsoft::Management::Deployment::PackageMatchField Field;
        winrt::Microsoft::Management::Deployment::PackageFieldMatchOption MatchOption;
    };

    struct TerminalPage : TerminalPageT<TerminalPage>
    {
    public:
        TerminalPage(TerminalApp::WindowProperties properties, const TerminalApp::ContentManager& manager);
        ~TerminalPage();

        // This implements shobjidl's IInitializeWithWindow, but due to a XAML Compiler bug we cannot
        // put it in our inheritance graph. https://github.com/microsoft/microsoft-ui-xaml/issues/3331
        STDMETHODIMP Initialize(HWND hwnd);

        void SetSettings(Microsoft::Terminal::Settings::Model::CascadiaSettings settings, bool needRefreshUI);

        void Create();
        Windows::UI::Xaml::Automation::Peers::AutomationPeer OnCreateAutomationPeer();

        bool ShouldImmediatelyHandoffToElevated(const Microsoft::Terminal::Settings::Model::CascadiaSettings& settings) const;
        void HandoffToElevated(const Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);

        hstring Title();

        void TitlebarClicked();
        void WindowVisibilityChanged(const bool showOrHide);

        float CalcSnappedDimension(const bool widthOrHeight, const float dimension) const;

        winrt::hstring ApplicationDisplayName();
        winrt::hstring ApplicationVersion();

        CommandPalette LoadCommandPalette();
        SuggestionsControl LoadSuggestionsUI();

        safe_void_coroutine RequestQuit();
        safe_void_coroutine CloseWindow();
        winrt::Microsoft::Terminal::Settings::Model::WindowLayout GetWindowLayout();
        void PersistState();
        std::vector<IPaneContent> Panes() const;

        void ToggleFocusMode();
        void ToggleFullscreen();
        void ToggleAlwaysOnTop();
        bool FocusMode() const;
        bool Fullscreen() const;
        bool AlwaysOnTop() const;
        bool ShowTabsFullscreen() const;
        void SetShowTabsFullscreen(bool newShowTabsFullscreen);
        void SetFullscreen(bool);
        void SetFocusMode(const bool inFocusMode);
        void Maximized(bool newMaximized);
        void RequestSetMaximized(bool newMaximized);

        void SetStartupActions(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions);
        void SetStartupWorkspaceState(uint32_t version,
                                      std::vector<winrt::guid> workspaceIds,
                                      std::vector<uint32_t> pinnedWorkspaceIndices,
                                      std::vector<Microsoft::Terminal::Settings::Model::AgentResumeInfo> agentResumeEntries,
                                      std::vector<winrt::guid> surfaceIds = {},
                                      std::vector<winrt::guid> activeSurfaceIds = {});
        void SetStartupWorkspaceSource(const winrt::guid& workspaceId, const winrt::guid& snapshotId, uint64_t revision);
        void AddStartupWorkspaceSource(const winrt::guid& workspaceId, const winrt::guid& snapshotId, uint64_t revision);
        void PrepareForWorkspaceSnapshotOpen(bool suppressStartupActions);
        void SetStartupConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection);

        static std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> ConvertExecuteCommandlineToActions(const Microsoft::Terminal::Settings::Model::ExecuteCommandlineArgs& args);

        winrt::TerminalApp::IDialogPresenter DialogPresenter() const;
        void DialogPresenter(winrt::TerminalApp::IDialogPresenter dialogPresenter);

        winrt::TerminalApp::TaskbarState TaskbarState() const;

        void ShowKeyboardServiceWarning() const;
        winrt::hstring KeyboardServiceDisabledText();

        void IdentifyWindow();
        void OpenWorkspaceSnapshot(const winrt::guid& snapshotId, bool replaceCurrentWorkspace);
        void ActionSaved(winrt::hstring input, winrt::hstring name, winrt::hstring keyChord);
        void ActionSaveFailed(winrt::hstring message);
        void ShowTerminalWorkingDirectory();

        safe_void_coroutine ProcessStartupActions(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions,
                                                   const winrt::hstring cwd = winrt::hstring{},
                                                   const winrt::hstring env = winrt::hstring{},
                                                   std::function<void()> completed = {},
                                                   std::function<void()> failed = {});
        safe_void_coroutine CreateTabFromConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection);

        TerminalApp::WindowProperties WindowProperties() const noexcept { return _WindowProperties; };

        bool CanDragDrop() const noexcept;
        bool IsRunningElevated() const noexcept;

        void OpenSettingsUI();
        void WindowActivated(const bool activated);
        bool FocusTab(const winrt::TerminalApp::Tab& tab);

        bool OnDirectKeyEvent(const uint32_t vkey, const uint8_t scanCode, const bool down);

        void AttachContent(Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::ActionAndArgs> args,
                           uint32_t tabIndex,
                           const winrt::guid& workspaceId,
                           const winrt::guid& sourceSnapshotId,
                           uint64_t sourceSnapshotRevision);
        void SendContentToOther(winrt::TerminalApp::RequestReceiveContentArgs args);

        uint32_t NumberOfTabs() const;

        til::property_changed_event PropertyChanged;

        // -------------------------------- WinRT Events ---------------------------------
        til::typed_event<IInspectable, IInspectable> TitleChanged;
        til::typed_event<IInspectable, IInspectable> CloseWindowRequested;
        til::typed_event<IInspectable, winrt::Windows::UI::Xaml::UIElement> SetTitleBarContent;
        til::typed_event<IInspectable, IInspectable> FocusModeChanged;
        til::typed_event<IInspectable, IInspectable> FullscreenChanged;
        til::typed_event<IInspectable, IInspectable> ChangeMaximizeRequested;
        til::typed_event<IInspectable, IInspectable> AlwaysOnTopChanged;
        til::typed_event<IInspectable, IInspectable> RaiseVisualBell;
        til::typed_event<IInspectable, IInspectable> SetTaskbarProgress;
        til::typed_event<IInspectable, IInspectable> Initialized;
        til::typed_event<IInspectable, IInspectable> IdentifyWindowsRequested;
        til::typed_event<IInspectable, winrt::TerminalApp::RenameWindowRequestedArgs> RenameWindowRequested;
        til::typed_event<IInspectable, IInspectable> SummonWindowRequested;
        til::typed_event<IInspectable, winrt::TerminalApp::SummonWindowByIdRequestedArgs> SummonWindowByIdRequested;
        til::typed_event<IInspectable, winrt::TerminalApp::Tab> FocusTabRequested;
        til::typed_event<IInspectable, winrt::Microsoft::Terminal::Control::WindowSizeChangedEventArgs> WindowSizeChanged;

        til::typed_event<IInspectable, IInspectable> OpenSystemMenu;
        til::typed_event<IInspectable, IInspectable> QuitRequested;
        til::typed_event<IInspectable, winrt::Microsoft::Terminal::Control::ShowWindowArgs> ShowWindowChanged;
        til::typed_event<Windows::Foundation::IInspectable, Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Terminal::Settings::Model::SettingsLoadWarnings>> ShowLoadWarningsDialog;

        til::typed_event<Windows::Foundation::IInspectable, winrt::TerminalApp::RequestMoveContentArgs> RequestMoveContent;
        til::typed_event<Windows::Foundation::IInspectable, winrt::TerminalApp::RequestReceiveContentArgs> RequestReceiveContent;

        til::typed_event<IInspectable, winrt::TerminalApp::LaunchPositionRequest> RequestLaunchPosition;
        til::typed_event<IInspectable, winrt::TerminalApp::WindowListRequest> RequestWindowList;
        til::typed_event<IInspectable, winrt::TerminalApp::OpenWindowRequestedArgs> RequestOpenWindow;
        til::typed_event<IInspectable, winrt::TerminalApp::WindowRequestedArgs> RequestNewWindow;

        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, TitlebarBrush, PropertyChanged.raise, nullptr);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, FrameBrush, PropertyChanged.raise, nullptr);

        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionName, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionKeyChord, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionCommandLine, PropertyChanged.raise, L"");

    private:
        friend struct TerminalPageT<TerminalPage>; // for Xaml to bind events
        std::optional<HWND> _hostingHwnd;

        // If you add controls here, but forget to null them either here or in
        // the ctor, you're going to have a bad time. It'll mysteriously fail to
        // activate the app.
        // ALSO: If you add any UIElements as roots here, make sure they're
        // updated in App::_ApplyTheme. The roots currently is _tabRow
        // (which is a root when the tabs are in the titlebar.)
        Microsoft::UI::Xaml::Controls::TabView _tabView{ nullptr };
        TerminalApp::TabRowControl _tabRow{ nullptr };
        Windows::UI::Xaml::Controls::Grid _tabContent{ nullptr };
        TerminalApp::WorkspaceSidebar _workspaceSidebar{ nullptr };
        TerminalApp::TaskPage _taskPage{ nullptr };
        TerminalApp::ConclusionSidebar _conclusionSidebar{ nullptr };
        Windows::UI::Xaml::Controls::ListView _workspaceList{ nullptr };
        Windows::UI::Xaml::Controls::Border _workspaceSurfaceTabBar{ nullptr };
        Windows::UI::Xaml::Controls::ListView _workspaceSurfaceTabs{ nullptr };
        Windows::UI::Xaml::Controls::Button _workspaceSurfaceNewTabButton{ nullptr };
        Windows::UI::Xaml::Controls::ListView _workspaceToolTabs{ nullptr };
        Windows::UI::Xaml::Controls::ListViewItem _workspaceTasksToolTab{ nullptr };
        Windows::UI::Xaml::Controls::ListViewItem _workspaceSummaryToolTab{ nullptr };
        WorkspaceToolPage _activeWorkspaceToolPage{ WorkspaceToolPage::None };
        tasks::TaskSurfacePolicy _taskSurfacePolicy;
        std::shared_ptr<tasks::TaskAgentSessionRegistry> _taskAgentSessions;
        uint64_t _nextTaskSurfaceCloseAuthorization{ 1 };
        Microsoft::UI::Xaml::Controls::SplitButton _newTabButton{ nullptr };
        Microsoft::UI::Xaml::Controls::SplitButton _workspaceNewTabButton{ nullptr };
        Windows::UI::Xaml::Controls::Button _workspaceNotificationButton{ nullptr };
        Windows::UI::Xaml::Controls::Button _workspaceSidebarToggleButton{ nullptr };
        Windows::UI::Xaml::Controls::MenuFlyout _workspaceFlyout{ nullptr };
        Windows::UI::Xaml::Controls::Button _workspaceDropdown{ nullptr };
        winrt::TerminalApp::ColorPickupFlyout _tabColorPicker{ nullptr };

        Microsoft::Terminal::Settings::Model::CascadiaSettings _settings{ nullptr };

        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _tabs;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel>::VectorChanged_revoker _workspaceItemsChangedRevoker;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _mruTabs;
        static winrt::com_ptr<Tab> _GetTabImpl(const TerminalApp::Tab& tab);
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel> _WorkspaceItems() const noexcept;
        WorkspaceSessionRestoreState& _SessionRestore() noexcept;
        const WorkspaceSessionRestoreState& _SessionRestore() const noexcept;
        WorkspaceSidebarController& _SidebarController() noexcept;
        const WorkspaceSidebarController& _SidebarController() const noexcept;

        void _UpdateTabIndices();

        TerminalApp::Tab _settingsTab{ nullptr };

        bool _isInFocusMode{ false };
        bool _isFullscreen{ false };
        bool _isMaximized{ false };
        bool _isAlwaysOnTop{ false };
        bool _showTabsFullscreen{ false };

        std::optional<uint32_t> _loadFromPersistedLayoutIdx{};

        bool _rearranging{ false };
        std::optional<int> _rearrangeFrom{};
        std::optional<int> _rearrangeTo{};
        bool _removing{ false };
        std::unique_ptr<WorkspaceFeatureSet> _workspaceFeatures;

        bool _activated{ false };
        bool _visible{ true };

        std::vector<std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>> _previouslyClosedPanesAndTabs{};

        uint32_t _systemRowsToScroll{ DefaultRowsToScroll };

        // use a weak reference to prevent circular dependency with AppLogic
        winrt::weak_ref<winrt::TerminalApp::IDialogPresenter> _dialogPresenter;

        winrt::com_ptr<AppKeyBindings> _bindings{ winrt::make_self<implementation::AppKeyBindings>() };
        winrt::com_ptr<ShortcutActionDispatch> _actionDispatch{ winrt::make_self<implementation::ShortcutActionDispatch>() };

        winrt::Windows::UI::Xaml::Controls::Grid::LayoutUpdated_revoker _layoutUpdatedRevoker;
        StartupState _startupState{ StartupState::NotInitialized };

        std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> _startupActions;
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _startupConnection{ nullptr };

        static constexpr uint32_t LegacyWorkspaceStateVersion{ 1 };
        static constexpr uint32_t CurrentWorkspaceStateVersion{ 2 };

        std::shared_ptr<Toast> _windowIdToast{ nullptr };
        std::shared_ptr<Toast> _actionSavedToast{ nullptr };
        std::shared_ptr<Toast> _actionSaveFailedToast{ nullptr };
        std::shared_ptr<Toast> _windowCwdToast{ nullptr };

        winrt::Windows::UI::Xaml::Controls::TextBox::LayoutUpdated_revoker _renamerLayoutUpdatedRevoker;
        int _renamerLayoutCount{ 0 };
        bool _renamerPressedEnter{ false };

        TerminalApp::WindowProperties _WindowProperties{ nullptr };
        PaneResources _paneResources;

        TerminalApp::ContentManager _manager{ nullptr };

        Microsoft::Terminal::Settings::Model::WindowSettings _currentWindowSettings() const;

        std::shared_ptr<TerminalSettingsCache> _terminalSettingsCache{};

        struct StashedDragData
        {
            winrt::com_ptr<winrt::TerminalApp::implementation::Tab> draggedTab{ nullptr };
            winrt::Windows::Foundation::Point dragOffset{ 0, 0 };
        } _stashed;

        safe_void_coroutine _NewTerminalByDrop(const Windows::Foundation::IInspectable&, winrt::Windows::UI::Xaml::DragEventArgs e);

        __declspec(noinline) CommandPalette _loadCommandPaletteSlowPath();
        bool _commandPaletteIs(winrt::Windows::UI::Xaml::Visibility visibility);
        __declspec(noinline) SuggestionsControl _loadSuggestionsElementSlowPath();
        bool _suggestionsControlIs(winrt::Windows::UI::Xaml::Visibility visibility);

        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowDialogHelper(const std::wstring_view& name);

        void _ShowAboutDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowConfirmCloseDialog(ConfirmCloseDialogKind kind);
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowCloseReadOnlyDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowMultiLinePasteWarningDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowLargePasteWarningDialog();

        void _CreateNewTabFlyout();
        void _CreateNewTabFlyoutForButton(const Microsoft::UI::Xaml::Controls::SplitButton& button);
        std::vector<winrt::Windows::UI::Xaml::Controls::MenuFlyoutItemBase> _CreateNewTabFlyoutItems(winrt::Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::NewTabMenuEntry> entries);
        winrt::Windows::UI::Xaml::Controls::IconElement _CreateNewTabFlyoutIcon(const winrt::hstring& icon);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutProfile(const Microsoft::Terminal::Settings::Model::Profile profile, int profileIndex, const winrt::hstring& iconPathOverride);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutAction(const winrt::hstring& actionId, const winrt::hstring& iconPathOverride);

        void _OpenNewTabDropdown();
        HRESULT _OpenNewTab(const Microsoft::Terminal::Settings::Model::INewContentArgs& newContentArgs);
        TerminalApp::Tab _CreateNewTabFromPane(std::shared_ptr<Pane> pane,
                                               uint32_t insertPosition = -1,
                                               winrt::guid workspaceId = {});

        std::wstring _evaluatePathForCwd(std::wstring_view path);

        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _CreateConnectionFromSettings(Microsoft::Terminal::Settings::Model::Profile profile,
                                                                                                            Microsoft::Terminal::Control::IControlSettings settings,
                                                                                                            bool inheritCursor,
                                                                                                            const winrt::guid& workspaceId);
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _duplicateConnectionForRestart(const TerminalApp::TerminalPaneContent& paneContent,
                                                                                                             const winrt::guid& workspaceId);
        void _restartPaneConnection(const TerminalApp::TerminalPaneContent&, const winrt::Windows::Foundation::IInspectable&);

        void _OpenNewWindow(const Microsoft::Terminal::Settings::Model::INewContentArgs& contentArgs);
        void _OpenWorkspaceWindow(const winrt::hstring name);

        void _OpenNewTerminalViaDropdown(const Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs);

        bool _displayingCloseDialog{ false };
        void _SettingsButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _CommandPaletteButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _AboutButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);

        void _KeyDownHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        static ::Microsoft::Terminal::Core::ControlKeyStates _GetPressedModifierKeys() noexcept;
        static void _ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept;
        void _HookupKeyBindings(const Microsoft::Terminal::Settings::Model::IActionMapView& actionMap) noexcept;
        void _RegisterActionCallbacks();

        void _UpdateTitle(const Tab& tab);
        void _UpdateTabIcon(Tab& tab);
        void _UpdateTabView();
        void _UpdateTabWidthMode();
        void _UpdateWorkspaceSidebar();
        void _UpdateWorkspaceTitleBar();
        void _UpdateWorkspaceListeningPortsTimer();
        void _RefreshWorkspaceListeningPorts();
        struct WorkspaceProcessRoots
        {
            winrt::guid WorkspaceId{};
            std::vector<uint32_t> RootProcessIds;
        };
        safe_void_coroutine _RefreshWorkspaceListeningPortsAsync(uint64_t generation,
                                                                 std::vector<WorkspaceProcessRoots> workspaces,
                                                                 std::vector<uint32_t> rootProcessIds);
        void _UpdateWorkspaceSidebarForWindowWidth(double width);
        void _Root_SizeChanged(const IInspectable& sender, const Windows::UI::Xaml::SizeChangedEventArgs& args);
        void _SyncWorkspaceSelection(const winrt::TerminalApp::Tab& tab);
        void _SetWorkspaceToolPage(WorkspaceToolPage page);
        safe_void_coroutine _HandleTaskSurface(tasks::TaskSurfaceDescriptor descriptor);
        void _CloseTaskSurface(std::wstring_view taskId,
                               uint64_t generation,
                               tasks::TaskSurfaceCloseOrigin origin,
                               std::string_view runtimeInstanceId = {});
        [[nodiscard]] tasks::TaskSurfaceId _TaskSurfaceId(const winrt::TerminalApp::Tab& tab) const noexcept;
        [[nodiscard]] tasks::TaskSurfaceId _TaskSurfaceId(const winrt::com_ptr<Tab>& tab) const noexcept;
        [[nodiscard]] bool _IsTaskOwnedTab(const winrt::TerminalApp::Tab& tab) const noexcept;
        [[nodiscard]] bool _IsTaskOwnedTab(const winrt::com_ptr<Tab>& tab) const noexcept;
        void _ApplyRestoredWorkspaceActiveSurfaces();
        bool _SelectWorkspace(const TerminalApp::WorkspaceItemViewModel& item);
        void _SynchronizeFlatTabOrder();
        std::optional<uint32_t> _FindWorkspaceIndex(const winrt::guid& workspaceId) const;
        void _RecordWorkspaceNavigation(const winrt::TerminalApp::Tab& tab);
        bool _NavigateWorkspaceHistory(bool forward);
        uint32_t _PinnedTabCount() const;
        int32_t _ClampTabIndexForPinnedState(const winrt::TerminalApp::Tab& tab, int32_t suggestedIndex) const;
        void _ToggleWorkspacePin(const winrt::TerminalApp::Tab& tab);
        void _WorkspacePinRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        void _WorkspaceCloseRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        void _WorkspaceCloseOthersRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        void _WorkspaceRenameRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        void _WorkspaceSurfaceTabs_SelectionChanged(const IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        void _WorkspaceSurfaceTabClose_Click(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _WorkspaceSurfaceNewTab_Click(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _WorkspaceToolTabs_SelectionChanged(const IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        safe_void_coroutine _WorkspaceConfigureAgentResumeRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        safe_void_coroutine _WorkspaceResumeAgentRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        void _WorkspaceClearAgentResumeRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        void _WorkspaceActivateNotificationRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        void _WorkspaceActivateNotificationItemRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceNotification& notification);
        void _WorkspaceMarkNotificationsReadRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        void _WorkspaceClearNotificationHistoryRequested(const TerminalApp::WorkspaceSidebar& sender, const TerminalApp::WorkspaceItemViewModel& item);
        void _WorkspaceMarkAllNotificationsReadRequested(const TerminalApp::WorkspaceSidebar& sender, const IInspectable& eventArgs);
        void _WorkspaceClearAllNotificationHistoryRequested(const TerminalApp::WorkspaceSidebar& sender, const IInspectable& eventArgs);
        TerminalApp::WorkspaceItemViewModel _WorkspaceItemForTab(const TerminalApp::Tab& tab) const;
        TerminalApp::WorkspaceItemViewModel _WorkspaceItemForNotification(const TerminalApp::WorkspaceNotification& notification) const;
        bool _RecordWorkspaceNotification(const winrt::com_ptr<Tab>& tab, const TerminalApp::IPaneContent& content, const winrt::hstring& title, const winrt::hstring& message);
        bool _FocusWorkspaceNotification(const TerminalApp::WorkspaceItemViewModel& item);
        bool _FocusWorkspaceNotification(const TerminalApp::WorkspaceItemViewModel& item, const TerminalApp::WorkspaceNotification& notification);
        bool _FocusLatestWorkspaceNotification();
        void _MarkWorkspaceNotificationsRead(const TerminalApp::WorkspaceItemViewModel& item);
        void _ClearWorkspaceNotificationHistory(const TerminalApp::WorkspaceItemViewModel& item);
        void _MarkAllWorkspaceNotificationsRead();
        void _ClearAllWorkspaceNotificationHistory();
        void _MarkWorkspaceNotificationsReadForActivePane(const winrt::com_ptr<Tab>& tab);
        void _RefreshWorkspaceNotifications();
        safe_void_coroutine _RenameWorkspace(winrt::TerminalApp::Tab tab);
        void _WorkspaceList_SelectionChanged(const IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        void _WorkspaceList_DragItemsStarting(const IInspectable& sender, const Windows::UI::Xaml::Controls::DragItemsStartingEventArgs& eventArgs);
        void _WorkspaceList_DragItemsCompleted(const IInspectable& sender, const Windows::UI::Xaml::Controls::DragItemsCompletedEventArgs& eventArgs);
        void _SetBackgroundImage(const winrt::Microsoft::Terminal::Settings::Model::IAppearanceConfig& newAppearance);

        void _DuplicateFocusedTab();
        void _DuplicateTab(const Tab& tab);

        safe_void_coroutine _ExportTab(const Tab& tab, winrt::hstring filepath);

        winrt::Windows::Foundation::IAsyncAction _HandleCloseTabRequested(winrt::TerminalApp::Tab tab, bool skipConfirmClose = false);
        void _CloseTabAtIndex(uint32_t index);
        void _RemoveTab(const winrt::TerminalApp::Tab& tab);
        safe_void_coroutine _RemoveTabs(const std::vector<winrt::TerminalApp::Tab> tabs);
        void _SaveWorkspaceIfNeeded();

        void _InitializeTab(winrt::com_ptr<Tab> newTabImpl,
                            uint32_t insertPosition = -1,
                            winrt::guid workspaceId = {});
        void _RegisterTerminalEvents(Microsoft::Terminal::Control::TermControl term);
        void _RegisterTabEvents(Tab& hostingTab);

        void _DismissTabContextMenus();
        void _FocusCurrentTab(const bool focusAlways);
        bool _HasMultipleTabs() const;

        void _SelectNextTab(const bool bMoveRight, const Windows::Foundation::IReference<Microsoft::Terminal::Settings::Model::TabSwitcherMode>& customTabSwitcherMode);
        bool _SelectTab(uint32_t tabIndex);
        bool _MoveFocus(const Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool _SwapPane(const Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool _MovePane(const Microsoft::Terminal::Settings::Model::MovePaneArgs args);
        bool _MoveTab(winrt::com_ptr<Tab> tab, const Microsoft::Terminal::Settings::Model::MoveTabArgs args);

        std::shared_ptr<ThrottledFunc<>> _adjustProcessPriorityThrottled;
        void _adjustProcessPriority() const;

        template<typename F>
        bool _ApplyToActiveControls(F f) const
        {
            if (const auto tab{ _GetFocusedTabImpl() })
            {
                if (const auto activePane = tab->GetActivePane())
                {
                    activePane->WalkTree([&](auto p) {
                        if (const auto& control{ p->GetTerminalControl() })
                        {
                            f(control);
                        }
                    });

                    return true;
                }
            }
            return false;
        }

        winrt::Microsoft::Terminal::Control::TermControl _GetActiveControl() const;
        std::optional<uint32_t> _GetFocusedTabIndex() const noexcept;
        std::optional<uint32_t> _GetTabIndex(const TerminalApp::Tab& tab) const noexcept;
        TerminalApp::Tab _GetFocusedTab() const noexcept;
        winrt::com_ptr<Tab> _GetFocusedTabImpl() const noexcept;
        TerminalApp::Tab _GetTabByTabViewItem(const IInspectable& tabViewItem) const noexcept;

        void _HandleClosePaneRequested(std::shared_ptr<Pane> pane);
        bool _ShouldWarnOnClose() const;
        bool _ShouldWarnOnCloseTab(const winrt::com_ptr<Tab>& tab) const;
        safe_void_coroutine _SetFocusedTab(const winrt::TerminalApp::Tab tab);
        safe_void_coroutine _CloseFocusedPane();
        safe_void_coroutine _ClosePanes(weak_ref<Tab> weakTab, std::vector<uint32_t> paneIds);
        void _CloseRemainingPanes(weak_ref<Tab> weakTab, std::vector<uint32_t> paneIds);
        winrt::Windows::Foundation::IAsyncOperation<bool> _PaneConfirmCloseReadOnly(std::shared_ptr<Pane> pane);
        void _AddPreviouslyClosedPaneOrTab(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>&& args);

        void _Scroll(ScrollDirection scrollDirection, const Windows::Foundation::IReference<uint32_t>& rowsToScroll);

        void _SplitPane(const winrt::com_ptr<Tab>& tab,
                        const Microsoft::Terminal::Settings::Model::SplitDirection splitType,
                        const float splitSize,
                        std::shared_ptr<Pane> newPane,
                        winrt::guid workspaceId = {});
        bool _ResizePane(const Microsoft::Terminal::Settings::Model::ResizeDirection& direction);
        void _ToggleSplitOrientation();

        void _ScrollPage(ScrollDirection scrollDirection);
        void _ScrollToBufferEdge(ScrollDirection scrollDirection);
        void _SetAcceleratorForMenuItem(Windows::UI::Xaml::Controls::MenuFlyoutItem& menuItem, const winrt::Microsoft::Terminal::Control::KeyChord& keyChord);

        safe_void_coroutine _PasteFromClipboardHandler(const IInspectable sender,
                                                       const Microsoft::Terminal::Control::PasteFromClipboardEventArgs eventArgs);

        safe_void_coroutine _OpenHyperlinkHandler(const IInspectable sender, const Microsoft::Terminal::Control::OpenHyperlinkEventArgs eventArgs);
        static bool _IsUriSupported(const winrt::Windows::Foundation::Uri& parsedUri);
        bool _IsUriConsideredSomewhatSafe(const winrt::Windows::Foundation::Uri& parsedUri) const;

        void _ShowCouldNotOpenDialog(winrt::hstring reason, winrt::hstring uri);
        bool _CopyText(bool dismissSelection, bool singleLine, bool withControlSequences, Microsoft::Terminal::Control::CopyFormat formats);

        safe_void_coroutine _SetTaskbarProgressHandler(const IInspectable sender, const IInspectable eventArgs);

        void _copyToClipboard(IInspectable, Microsoft::Terminal::Control::WriteToClipboardEventArgs args) const;
        void _PasteText();

        safe_void_coroutine _ControlNoticeRaisedHandler(const IInspectable sender, const Microsoft::Terminal::Control::NoticeEventArgs eventArgs);
        void _ShowControlNoticeDialog(const winrt::hstring& title, const winrt::hstring& message);

        safe_void_coroutine _LaunchSettings(const Microsoft::Terminal::Settings::Model::SettingsTarget target);

        void _TabDragStarted(const IInspectable& sender, const IInspectable& eventArgs);
        void _TabDragCompleted(const IInspectable& sender, const IInspectable& eventArgs);

        // BODGY: WinUI's TabView has a broken close event handler:
        // If the close button is disabled, middle-clicking the tab raises no close
        // event. Because that's dumb, we implement our own middle-click handling.
        // `_tabItemMiddleClickHookEnabled` is true whenever the close button is hidden,
        // and that enables all of the rest of this machinery (and this workaround).
        bool _tabItemMiddleClickHookEnabled = false;
        bool _tabItemMiddleClickExited = false;
        PointerEntered_revoker _tabItemMiddleClickPointerEntered;
        PointerExited_revoker _tabItemMiddleClickPointerExited;
        PointerCaptureLost_revoker _tabItemMiddleClickPointerCaptureLost;
        void _OnTabPointerPressed(const IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        safe_void_coroutine _OnTabPointerReleasedCloseTab(IInspectable sender);

        void _OnTabSelectionChanged(const IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        void _OnTabItemsChanged(const IInspectable& sender, const Windows::Foundation::Collections::IVectorChangedEventArgs& eventArgs);
        void _OnTabCloseRequested(const IInspectable& sender, const Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs& eventArgs);
        void _OnFirstLayout(const IInspectable& sender, const IInspectable& eventArgs);
        void _UpdatedSelectedTab(const winrt::TerminalApp::Tab& tab);
        void _UpdateBackground(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile);

        void _OnDispatchCommandRequested(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& command);
        void _OnCommandLineExecutionRequested(const IInspectable& sender, const winrt::hstring& commandLine);
        void _OnSwitchToTabRequested(const IInspectable& sender, const winrt::TerminalApp::Tab& tab);

        void _Find(const Tab& tab);

        winrt::Microsoft::Terminal::Control::TermControl _CreateNewControlAndContent(const winrt::Microsoft::Terminal::Settings::TerminalSettingsCreateResult& settings,
                                                                                     const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& connection);
        winrt::Microsoft::Terminal::Control::TermControl _SetupControl(const winrt::Microsoft::Terminal::Control::TermControl& term);
        winrt::Microsoft::Terminal::Control::TermControl _AttachControlToContent(const uint64_t& contentGuid);

        TerminalApp::IPaneContent _makeSettingsContent();
        std::shared_ptr<Pane> _MakeTerminalPane(const Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs = nullptr,
                                                const winrt::TerminalApp::Tab& sourceTab = nullptr,
                                                winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr,
                                                bool allowDebugTap = true,
                                                const winrt::guid& workspaceId = {});
        std::shared_ptr<Pane> _MakePane(const Microsoft::Terminal::Settings::Model::INewContentArgs& newContentArgs = nullptr,
                                        const winrt::TerminalApp::Tab& sourceTab = nullptr,
                                        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr,
                                        bool allowDebugTap = true,
                                        const winrt::guid& workspaceId = {});

        winrt::guid _ReserveWorkspaceIdForNewTab() const;
        winrt::guid _WorkspaceIdForNewTab() const;
        void _OpenNewWorkspace();
        void _OpenNewWorkspaceSurface(const TerminalApp::WorkspaceItemViewModel& item);
        winrt::guid _WorkspaceIdForTab(const TerminalApp::Tab& tab) const;
        winrt::guid _WorkspaceIdForPaneContent(const TerminalApp::TerminalPaneContent& paneContent) const;
        static winrt::Windows::Foundation::Collections::IMapView<winrt::hstring, winrt::hstring> _BuildWorkspaceEnvironment(
            const winrt::Windows::Foundation::Collections::IMapView<winrt::hstring, winrt::hstring>& profileEnvironment,
            const winrt::guid& workspaceId,
            std::wstring_view pipeName,
            uint64_t windowId);

        void _RefreshUIForSettingsReload();

        void _SetNewTabButtonColor(til::color color, til::color accentColor);
        void _ClearNewTabButtonColor();

        safe_void_coroutine _CompleteInitialization();

        void _FocusActiveControl(IInspectable sender, IInspectable eventArgs);

        void _UnZoomIfNeeded();

        static int _ComputeScrollDelta(ScrollDirection scrollDirection, const uint32_t rowsToScroll);
        static uint32_t _ReadSystemRowsToScroll();

        void _UpdateMRUTab(const winrt::TerminalApp::Tab& tab);

        void _TryMoveTab(const uint32_t currentTabIndex, const int32_t suggestedNewTabIndex, bool selectMovedTab = true);

        void _PreviewAction(const Microsoft::Terminal::Settings::Model::ActionAndArgs& args);
        void _PreviewActionHandler(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& args);
        void _EndPreview();
        void _RunRestorePreviews();
        void _PreviewColorScheme(const Microsoft::Terminal::Settings::Model::SetColorSchemeArgs& args);
        void _PreviewAdjustOpacity(const Microsoft::Terminal::Settings::Model::AdjustOpacityArgs& args);
        void _PreviewSendInput(const Microsoft::Terminal::Settings::Model::SendInputArgs& args);

        winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs _lastPreviewedAction{ nullptr };
        std::vector<std::function<void()>> _restorePreviewFuncs{};

        HRESULT _OnNewConnection(const winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection& connection);
        void _HandleToggleInboundPty(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);

        void _WindowRenamerActionClick(const IInspectable& sender, const IInspectable& eventArgs);
        void _RequestWindowRename(const winrt::hstring& newName);
        void _WindowRenamerKeyDown(const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        void _WindowRenamerKeyUp(const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);

        void _UpdateTeachingTipTheme(winrt::Windows::UI::Xaml::FrameworkElement element);

        winrt::Microsoft::Terminal::Settings::Model::Profile GetClosestProfileForDuplicationOfProfile(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile) const noexcept;

        bool _maybeElevate(const winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs,
                           const winrt::Microsoft::Terminal::Settings::TerminalSettingsCreateResult& controlSettings,
                           const winrt::Microsoft::Terminal::Settings::Model::Profile& profile);
        void _OpenElevatedWT(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs);

        safe_void_coroutine _ConnectionStateChangedHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args);
        void _CloseOnExitInfoDismissHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args) const;
        void _KeyboardServiceWarningInfoDismissHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args) const;
        static bool _IsMessageDismissed(const winrt::Microsoft::Terminal::Settings::Model::InfoBarMessage& message);
        static void _DismissMessage(const winrt::Microsoft::Terminal::Settings::Model::InfoBarMessage& message);

        void _updateThemeColors();
        void _updateAllTabCloseButtons();
        void _updatePaneResources(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme);

        safe_void_coroutine _ControlCompletionsChangedHandler(const winrt::Windows::Foundation::IInspectable sender, const winrt::Microsoft::Terminal::Control::CompletionsChangedEventArgs args);

        void _OpenSuggestions(const Microsoft::Terminal::Control::TermControl& sender, Windows::Foundation::Collections::IVector<winrt::Microsoft::Terminal::Settings::Model::Command> commandsCollection, winrt::TerminalApp::SuggestionsMode mode, winrt::hstring filterText);

        void _ShowWindowChangedHandler(const IInspectable sender, const winrt::Microsoft::Terminal::Control::ShowWindowArgs args);
        Windows::Foundation::IAsyncAction _SearchMissingCommandHandler(const IInspectable sender, const winrt::Microsoft::Terminal::Control::SearchMissingCommandEventArgs args);
        static Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Management::Deployment::MatchResult>> _FindPackageAsync(hstring query);

        void _WindowSizeChanged(const IInspectable sender, const winrt::Microsoft::Terminal::Control::WindowSizeChangedEventArgs args);
        void _windowPropertyChanged(const IInspectable& sender, const winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs& args);

        void _onTabDragStarting(const winrt::Microsoft::UI::Xaml::Controls::TabView& sender, const winrt::Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs& e);
        void _onTabStripDragOver(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::DragEventArgs& e);
        void _onTabStripDrop(winrt::Windows::Foundation::IInspectable sender, winrt::Windows::UI::Xaml::DragEventArgs e);
        void _onTabDroppedOutside(winrt::Windows::Foundation::IInspectable sender, winrt::Microsoft::UI::Xaml::Controls::TabViewTabDroppedOutsideEventArgs e);

        void _DetachPaneFromWindow(std::shared_ptr<Pane> pane);
        void _DetachTabFromWindow(const winrt::com_ptr<Tab>& tabImpl);
        void _MoveContent(std::vector<winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs>&& actions,
                          const winrt::hstring& windowName,
                          const uint32_t tabIndex,
                          const std::optional<winrt::Windows::Foundation::Point>& dragPoint = std::nullopt,
                          const winrt::guid& workspaceId = {},
                          const winrt::guid& sourceSnapshotId = {},
                          uint64_t sourceSnapshotRevision = 0);
        void _sendDraggedTabToWindow(const winrt::hstring& windowId, const uint32_t tabIndex, std::optional<winrt::Windows::Foundation::Point> dragPoint);

        void _PopulateContextMenu(const Microsoft::Terminal::Control::TermControl& control, const Microsoft::UI::Xaml::Controls::CommandBarFlyout& sender, const bool withSelection);
        void _PopulateQuickFixMenu(const Microsoft::Terminal::Control::TermControl& control, const Windows::UI::Xaml::Controls::MenuFlyout& sender);
        void _DrainPendingWorkspaceContent();
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _CreateRunAsAdminFlyout(int profileIndex);

        winrt::Microsoft::Terminal::Control::TermControl _senderOrActiveControl(const winrt::Windows::Foundation::IInspectable& sender);
        winrt::com_ptr<Tab> _senderOrFocusedTab(const IInspectable& sender);

        void _activePaneChanged(winrt::TerminalApp::Tab tab, Windows::Foundation::IInspectable args);
        safe_void_coroutine _doHandleSuggestions(Microsoft::Terminal::Settings::Model::SuggestionsArgs realArgs);

        void _SendDesktopNotification(const winrt::hstring& tabTitle, const winrt::hstring& body, const winrt::com_ptr<Tab>& tab, const winrt::TerminalApp::IPaneContent& content);

#pragma region ActionHandlers
        // These are all defined in AppActionHandlers.cpp
#define ON_ALL_ACTIONS(action) DECLARE_ACTION_HANDLER(action);
        ALL_SHORTCUT_ACTIONS
        INTERNAL_SHORTCUT_ACTIONS
#undef ON_ALL_ACTIONS
#pragma endregion

        friend class TerminalAppLocalTests::TabTests;
        friend class TerminalAppLocalTests::SettingsTests;
        friend class WorkspaceAutomationController;
        friend class AgentHistoryCoordinator;
        friend class WorkspaceSnapshotCoordinator;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TerminalPage);
    BASIC_FACTORY(WindowListEntry);
}
