// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceSidebarRow.g.h"
#include "WorkspaceSidebarRowTemplateSelector.g.h"

namespace winrt::TerminalApp::implementation
{
    struct WorkspaceSidebarRowData
    {
        TerminalApp::WorkspaceSidebarRowKind Kind{ TerminalApp::WorkspaceSidebarRowKind::Workspace };
        TerminalApp::WorkspaceItemViewModel Workspace{ nullptr };
        TerminalApp::Tab Surface{ nullptr };
        uint32_t PaneId{};

        winrt::hstring Title;
        winrt::hstring Subtitle;
        winrt::hstring DirectoryDisplay;
        winrt::hstring WorkingDirectory;
        winrt::hstring GitBranch;
        winrt::hstring GitRepositoryRoot;
        winrt::hstring ListeningPortsDisplay;
        winrt::hstring ListeningPortsTooltip;
        winrt::hstring StatusText;
        winrt::hstring UnreadBadgeText;
        winrt::hstring LatestNotification;
        winrt::hstring SurfaceCountText;
        winrt::hstring PaneCountBadgeText;
        winrt::hstring SwitchShortcutHintText;
        winrt::hstring TreeBranchGlyph;
        winrt::hstring ChevronGlyph;
        winrt::hstring PinIconGlyph;
        winrt::hstring PinActionText;
        winrt::hstring PinAutomationName;
        winrt::hstring NotificationAutomationName;
        winrt::hstring NotificationHistoryCountText;
        winrt::hstring AutomationName;

        bool IsPinned{};
        bool IsSelected{};
        bool IsActive{};
        bool IsCollapsed{};
        bool IsRunning{};
        bool HasAttention{};
        bool ShowCollapsedUnread{};
        bool HasAgentResume{};
        bool IsVimActive{};
        bool IsTerminalIcon{ true };
        bool IsClaudeIcon{};
        bool IsCodexIcon{};
        bool IsAgyIcon{};
    };

    struct WorkspaceSidebarRow : WorkspaceSidebarRowT<WorkspaceSidebarRow>
    {
        WorkspaceSidebarRow() = default;

        TerminalApp::WorkspaceSidebarRowKind Kind() const noexcept { return _kind; }
        TerminalApp::WorkspaceItemViewModel Workspace() const noexcept { return _workspace; }
        TerminalApp::Tab Surface() const noexcept { return _surface; }
        uint32_t PaneId() const noexcept { return _paneId; }

        winrt::hstring Title() const noexcept { return _title; }
        winrt::hstring Subtitle() const noexcept { return _subtitle; }
        winrt::hstring DirectoryDisplay() const noexcept { return _directoryDisplay; }
        winrt::hstring WorkingDirectory() const noexcept { return _workingDirectory; }
        winrt::hstring GitBranch() const noexcept { return _gitBranch; }
        winrt::hstring GitRepositoryRoot() const noexcept { return _gitRepositoryRoot; }
        winrt::hstring ListeningPortsDisplay() const noexcept { return _listeningPortsDisplay; }
        winrt::hstring ListeningPortsTooltip() const noexcept { return _listeningPortsTooltip; }
        winrt::hstring StatusText() const noexcept { return _statusText; }
        winrt::hstring UnreadBadgeText() const noexcept { return _unreadBadgeText; }
        winrt::hstring LatestNotification() const noexcept { return _latestNotification; }
        winrt::hstring SurfaceCountText() const noexcept { return _surfaceCountText; }
        winrt::hstring PaneCountBadgeText() const noexcept { return _paneCountBadgeText; }
        winrt::hstring SwitchShortcutHintText() const noexcept { return _switchShortcutHintText; }
        winrt::hstring TreeBranchGlyph() const noexcept { return _treeBranchGlyph; }
        winrt::hstring ChevronGlyph() const noexcept { return _chevronGlyph; }
        winrt::hstring PinIconGlyph() const noexcept { return _pinIconGlyph; }
        winrt::hstring PinActionText() const noexcept { return _pinActionText; }
        winrt::hstring PinAutomationName() const noexcept { return _pinAutomationName; }
        winrt::hstring NotificationAutomationName() const noexcept { return _notificationAutomationName; }
        winrt::hstring NotificationHistoryCountText() const noexcept { return _notificationHistoryCountText; }
        winrt::hstring AutomationName() const noexcept { return _automationName; }

        bool IsPinned() const noexcept { return _isPinned; }
        bool IsSelected() const noexcept { return _isSelected; }
        bool IsActive() const noexcept { return _isActive; }
        bool IsCollapsed() const noexcept { return _isCollapsed; }
        bool IsRunning() const noexcept { return _isRunning; }
        bool HasAttention() const noexcept { return _hasAttention; }
        bool IsIdle() const noexcept { return !_isRunning && !_hasAttention; }
        bool ShowCollapsedUnread() const noexcept { return _showCollapsedUnread; }
        bool HasAgentResume() const noexcept { return _hasAgentResume; }
        bool IsVimActive() const noexcept { return _isVimActive; }
        bool IsTerminalIcon() const noexcept { return _isTerminalIcon; }
        bool IsClaudeIcon() const noexcept { return _isClaudeIcon; }
        bool IsCodexIcon() const noexcept { return _isCodexIcon; }
        bool IsAgyIcon() const noexcept { return _isAgyIcon; }

        til::property_changed_event PropertyChanged;

        void SetSwitchShortcutHintText(winrt::hstring value)
        {
            if (_switchShortcutHintText != value)
            {
                _switchShortcutHintText = std::move(value);
                PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SwitchShortcutHintText" });
            }
        }

        bool HasSameIdentityAs(const WorkspaceSidebarRow& other) const noexcept;
        bool HasSameIdentityAs(const WorkspaceSidebarRowData& other) const noexcept;
        bool UpdateFrom(const WorkspaceSidebarRow& next);
        bool UpdateFrom(const WorkspaceSidebarRowData& next);
        void ApplyInitialData(WorkspaceSidebarRowData&& next) noexcept;

        // Internal population helpers
        TerminalApp::WorkspaceSidebarRowKind _kind{ TerminalApp::WorkspaceSidebarRowKind::Workspace };
        TerminalApp::WorkspaceItemViewModel _workspace{ nullptr };
        TerminalApp::Tab _surface{ nullptr };
        uint32_t _paneId{};

        winrt::hstring _title;
        winrt::hstring _subtitle;
        winrt::hstring _directoryDisplay;
        winrt::hstring _workingDirectory;
        winrt::hstring _gitBranch;
        winrt::hstring _gitRepositoryRoot;
        winrt::hstring _listeningPortsDisplay;
        winrt::hstring _listeningPortsTooltip;
        winrt::hstring _statusText;
        winrt::hstring _unreadBadgeText;
        winrt::hstring _latestNotification;
        winrt::hstring _surfaceCountText;
        winrt::hstring _paneCountBadgeText;
        winrt::hstring _switchShortcutHintText;
        winrt::hstring _treeBranchGlyph;
        winrt::hstring _chevronGlyph;
        winrt::hstring _pinIconGlyph;
        winrt::hstring _pinActionText;
        winrt::hstring _pinAutomationName;
        winrt::hstring _notificationAutomationName;
        winrt::hstring _notificationHistoryCountText;
        winrt::hstring _automationName;

        bool _isPinned{};
        bool _isSelected{};
        bool _isActive{};
        bool _isCollapsed{};
        bool _isRunning{};
        bool _hasAttention{};
        bool _showCollapsedUnread{};
        bool _hasAgentResume{};
        bool _isVimActive{};
        bool _isTerminalIcon{ true };
        bool _isClaudeIcon{};
        bool _isCodexIcon{};
        bool _isAgyIcon{};
    };

    struct WorkspaceSidebarRowTemplateSelector : WorkspaceSidebarRowTemplateSelectorT<WorkspaceSidebarRowTemplateSelector>
    {
        WorkspaceSidebarRowTemplateSelector() = default;

        Windows::UI::Xaml::DataTemplate SelectTemplateCore(const Windows::Foundation::IInspectable& item,
                                                           const Windows::UI::Xaml::DependencyObject& container);
        Windows::UI::Xaml::DataTemplate SelectTemplateCore(const Windows::Foundation::IInspectable& item);

        WINRT_PROPERTY(Windows::UI::Xaml::DataTemplate, WorkspaceTemplate, nullptr);
        WINRT_PROPERTY(Windows::UI::Xaml::DataTemplate, SurfaceTemplate, nullptr);
        WINRT_PROPERTY(Windows::UI::Xaml::DataTemplate, PaneTemplate, nullptr);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(WorkspaceSidebarRowTemplateSelector);
}
