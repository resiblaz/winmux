// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceSidebar.g.h"
#include "WorkspaceSidebarRow.h"
#include "WorkspaceSidebarTreeModel.h"
#include "WorkspaceRemoteBridge.h"

namespace winrt::TerminalApp::implementation
{
    struct SidebarProcessState
    {
        SidebarIconKind Icon{ SidebarIconKind::Terminal };
        bool HasActiveChild{ false };
        bool AllowsTitleFallbackWhenIdle{ false };
        std::wstring ActiveProcessName{};

        bool operator==(const SidebarProcessState&) const noexcept = default;
    };

    struct WorkspaceSidebar : WorkspaceSidebarT<WorkspaceSidebar>
    {
        WorkspaceSidebar();

        bool IsExpanded() const noexcept;
        void IsExpanded(bool value);
        double ExpandedWidth() const noexcept;
        void ExpandedWidth(double value);
        void SetWindowNarrow(bool value);
        bool IsWindowNarrow() const noexcept;
        bool IsEffectivelyExpanded() const noexcept;
        static double NormalizeExpandedWidth(double value) noexcept;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> NotificationHistory() const noexcept;
        void RefreshNotifications();
        void OpenNotificationCenter(const Windows::UI::Xaml::FrameworkElement& target);
        void ShowSwitchHints(bool value, bool surfaceHints = false);

        void SetWorkspaces(const Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel>& workspaces);
        void SetActiveSelection(const TerminalApp::Tab& activeTab, std::optional<uint32_t> activePaneId);
        void UpdateProcessStates(std::unordered_map<uint32_t, SidebarProcessState> processStates);
        const std::unordered_map<uint32_t, SidebarProcessState>& ProcessStates() const noexcept { return _processStates; }
        void RefreshTree();
        void ScheduleRefreshTree();
        void SetSearchQueryForTests(winrt::hstring query);
        void SetFilterModeForTests(SidebarFilterMode mode);
        bool ToggleWorkspaceCollapsed(const winrt::guid& workspaceId);
        bool IsWorkspaceCollapsed(const winrt::guid& workspaceId) const noexcept;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceSidebarRow> TreeRows() const noexcept;

        static constexpr double NarrowWindowThreshold{ 640.0 };

        til::property_changed_event PropertyChanged;
        til::typed_event<TerminalApp::WorkspaceSidebar, Windows::Foundation::IInspectable> NewWorkspaceRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> NewSurfaceRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceSidebarRow> ActivateRowRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::Tab> CloseSurfaceRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::Tab> RenameSurfaceRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::Tab> SplitSurfaceRequested;
        til::event<winrt::delegate<TerminalApp::WorkspaceItemViewModel, uint32_t>> ReorderWorkspaceRequested;
        til::event<winrt::delegate<TerminalApp::Tab, TerminalApp::WorkspaceItemViewModel, std::optional<uint32_t>>> MoveSurfaceRequested;

        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> PinRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> CloseRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> CloseOthersRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> RenameRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> SaveSnapshotRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ConfigureAgentResumeRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ResumeAgentRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ClearAgentResumeRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ActivateNotificationRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceNotification> ActivateNotificationItemRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> MarkNotificationsReadRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> ClearNotificationHistoryRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, Windows::Foundation::IInspectable> MarkAllNotificationsReadRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, Windows::Foundation::IInspectable> ClearAllNotificationHistoryRequested;

        void _Toggle_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CollapseAll_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CreateWorkspace_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _SearchBox_TextChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::TextChangedEventArgs& args);
        void _SearchClear_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _FilterAll_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _FilterAttention_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _FilterRunning_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Resize_DragDelta(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::Primitives::DragDeltaEventArgs& args);
        void _Resize_PreviewKeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& args);
        void _WorkspaceList_KeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& args);
        void _WorkspaceList_ItemClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::ItemClickEventArgs& args);
        void _WorkspaceList_DragItemsStarting(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::DragItemsStartingEventArgs& args);
        void _WorkspaceList_DragItemsCompleted(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::DragItemsCompletedEventArgs& args);
        void _TreeRow_DragOver(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::DragEventArgs& args);
        void _TreeRow_Drop(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::DragEventArgs& args);
        void _WorkspaceItem_PointerEntered(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        void _WorkspaceItem_PointerExited(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        void _WorkspaceItemCloseButton_PointerExited(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        void _SurfaceItem_PointerEntered(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        void _SurfaceItem_PointerExited(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        void _Chevron_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _AddSurface_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _OpenDirectory_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CopyPath_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CloseSurface_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RenameSurface_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _SplitSurface_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Pin_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Close_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CloseOthers_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Rename_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _SaveSnapshot_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ConfigureAgentResume_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ResumeAgent_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ClearAgentResume_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Notification_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _MarkNotificationsRead_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ClearNotificationHistory_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _NotificationCenter_Opening(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& args);
        void _NotificationHistory_ItemClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::ItemClickEventArgs& args);
        void _NotificationHistory_KeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& args);
        void _MarkAllNotificationsRead_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ClearAllNotificationHistory_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RemoteBridgeFlyout_Opening(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& args);
        void _RemoteBridgeToggle_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RemoteBridgeRefresh_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RemoteBridgeCopyUri_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RemoteBridgeAutoStart_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RemoteBridgeHooks_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RemoteBridgeTunnel_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RemoteBridgeRevokeDevice_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

    private:
        static constexpr double MinimumExpandedWidth{ 152.0 };
        static constexpr double MaximumExpandedWidth{ 360.0 };
        static constexpr double DefaultExpandedWidth{ 232.0 };
        static constexpr double PreviousDefaultExpandedWidth{ 200.0 };
        static constexpr double LegacyDefaultExpandedWidth{ 280.0 };
        static constexpr double CompactLegacyDefaultExpandedWidth{ 176.0 };
        static constexpr double CollapsedWidth{ 0.0 };

        struct SurfaceSubscriptions
        {
            Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker TabChanged;
            Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker StatusChanged;
            winrt::event_token PaneTreeChangedToken{};
            winrt::weak_ref<TerminalApp::Tab> WeakTab;
        };

        struct WorkspaceSubscriptions
        {
            Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker WorkspaceChanged;
            Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab>::VectorChanged_revoker SurfacesChanged;
            std::vector<SurfaceSubscriptions> Surfaces;
        };

        bool _isExpanded{ true };
        bool _isWindowNarrow{ false };
        bool _layoutElementsReady{ false };
        bool _layoutAnimationsEnabled{ false };
        bool _showWorkspaceSwitchHints{ false };
        bool _showSurfaceSwitchHints{ false };
        bool _refreshScheduled{ false };
        bool _syncingSelection{ false };
        double _expandedWidth{ DefaultExpandedWidth };
        Windows::UI::Xaml::Media::Animation::Storyboard _layoutStoryboard{ nullptr };
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> _notificationHistory;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel> _workspaces{ nullptr };
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel>::VectorChanged_revoker _workspacesChangedRevoker;
        std::vector<WorkspaceSubscriptions> _subscriptions;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceSidebarRow> _treeRows;
        WorkspaceSidebarTreeModel _treeModel;
        winrt::weak_ref<TerminalApp::Tab> _activeTab;
        std::optional<uint32_t> _activePaneId;
        struct FilterHeaderState
        {
            uint32_t TotalSurfaceCount{ 0 };
            uint32_t AttentionSurfaceCount{ 0 };
            uint32_t RunningSurfaceCount{ 0 };
            SidebarFilterMode FilterMode{ SidebarFilterMode::All };
            bool AnyWorkspaceExpanded{ false };
            bool ShowEmptyState{ false };

            bool operator==(const FilterHeaderState&) const noexcept = default;
        };

        std::unordered_map<uint32_t, SidebarProcessState> _processStates;
        TerminalApp::WorkspaceSidebarRow _dragRow{ nullptr };
        std::optional<FilterHeaderState> _lastFilterHeaderState;
        WorkspaceRemoteBridge _remoteBridge;
        RemoteBridgeStatus _remoteBridgeStatus;
        std::string _renderedQrKey;
        bool _remoteBridgeBusy{ false };
        uint64_t _remoteBridgeGeneration{ 0 };

        void _ApplyLayout();
        winrt::fire_and_forget _AutoStartRemoteBridgeAsync();
        winrt::fire_and_forget _RefreshRemoteBridgeAsync(bool refreshCode);
        winrt::fire_and_forget _ToggleRemoteBridgeAsync();
        winrt::fire_and_forget _ToggleRemoteHooksAsync();
        winrt::fire_and_forget _ToggleRemoteTunnelAsync();
        winrt::fire_and_forget _RevokeRemoteDeviceAsync(std::wstring deviceId);
        void _ApplyRemoteBridgeStatus(const RemoteBridgeStatus& status, bool busy);
        void _SetExpandedContentVisibility(Windows::UI::Xaml::Visibility visibility);
        void _ActivateNotification(const TerminalApp::WorkspaceNotification& notification);
        void _SetWorkspaceItemHoverVisibility(const Windows::Foundation::IInspectable& sender, Windows::UI::Xaml::Visibility visibility) const;
        void _SetSurfaceItemHoverVisibility(const Windows::Foundation::IInspectable& sender, Windows::UI::Xaml::Visibility visibility) const;
        TerminalApp::WorkspaceSidebarRow _RowFromSender(const Windows::Foundation::IInspectable& sender) const;
        TerminalApp::WorkspaceItemViewModel _ItemFromSender(const Windows::Foundation::IInspectable& sender) const;
        TerminalApp::Tab _SurfaceFromSender(const Windows::Foundation::IInspectable& sender) const;
        void _RebindWorkspaceSubscriptions();
        void _UpdateFilterHeader(const SidebarProjection& projection);
        void _SyncListSelection();
        void _SelectRelativeRow(int32_t delta);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(WorkspaceSidebar);
}
