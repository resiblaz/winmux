// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceSidebarRow.h"
#include "WorkspaceSidebarRow.g.cpp"
#include "WorkspaceSidebarRowTemplateSelector.g.cpp"

namespace winrt::TerminalApp::implementation
{
    bool WorkspaceSidebarRow::HasSameIdentityAs(const WorkspaceSidebarRow& other) const noexcept
    {
        return _kind == other._kind &&
               _workspace == other._workspace &&
               _surface == other._surface &&
               _paneId == other._paneId;
    }

    bool WorkspaceSidebarRow::HasSameIdentityAs(const WorkspaceSidebarRowData& other) const noexcept
    {
        return _kind == other.Kind &&
               _workspace == other.Workspace &&
               _surface == other.Surface &&
               _paneId == other.PaneId;
    }

    void WorkspaceSidebarRow::ApplyInitialData(WorkspaceSidebarRowData&& next) noexcept
    {
        _kind = next.Kind;
        _workspace = std::move(next.Workspace);
        _surface = std::move(next.Surface);
        _paneId = next.PaneId;

        _title = std::move(next.Title);
        _subtitle = std::move(next.Subtitle);
        _directoryDisplay = std::move(next.DirectoryDisplay);
        _workingDirectory = std::move(next.WorkingDirectory);
        _gitBranch = std::move(next.GitBranch);
        _gitRepositoryRoot = std::move(next.GitRepositoryRoot);
        _listeningPortsDisplay = std::move(next.ListeningPortsDisplay);
        _listeningPortsTooltip = std::move(next.ListeningPortsTooltip);
        _statusText = std::move(next.StatusText);
        _unreadBadgeText = std::move(next.UnreadBadgeText);
        _latestNotification = std::move(next.LatestNotification);
        _surfaceCountText = std::move(next.SurfaceCountText);
        _paneCountBadgeText = std::move(next.PaneCountBadgeText);
        _switchShortcutHintText = std::move(next.SwitchShortcutHintText);
        _treeBranchGlyph = std::move(next.TreeBranchGlyph);
        _chevronGlyph = std::move(next.ChevronGlyph);
        _pinIconGlyph = std::move(next.PinIconGlyph);
        _pinActionText = std::move(next.PinActionText);
        _pinAutomationName = std::move(next.PinAutomationName);
        _notificationAutomationName = std::move(next.NotificationAutomationName);
        _notificationHistoryCountText = std::move(next.NotificationHistoryCountText);
        _automationName = std::move(next.AutomationName);

        _isPinned = next.IsPinned;
        _isSelected = next.IsSelected;
        _isActive = next.IsActive;
        _isCollapsed = next.IsCollapsed;
        _isRunning = next.IsRunning;
        _hasAttention = next.HasAttention;
        _showCollapsedUnread = next.ShowCollapsedUnread;
        _hasAgentResume = next.HasAgentResume;
        _isVimActive = next.IsVimActive;
        _isTerminalIcon = next.IsTerminalIcon;
        _isClaudeIcon = next.IsClaudeIcon;
        _isCodexIcon = next.IsCodexIcon;
        _isAgyIcon = next.IsAgyIcon;
    }

    bool WorkspaceSidebarRow::UpdateFrom(const WorkspaceSidebarRowData& next)
    {
        bool anyChanged = false;
        const auto updateField = [&](auto& field, const auto& nextValue, const std::wstring_view propertyName) {
            if (field != nextValue)
            {
                field = nextValue;
                anyChanged = true;
                PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ propertyName });
            }
        };

        const bool wasIdle = IsIdle();

        updateField(_title, next.Title, L"Title");
        updateField(_subtitle, next.Subtitle, L"Subtitle");
        updateField(_directoryDisplay, next.DirectoryDisplay, L"DirectoryDisplay");
        updateField(_workingDirectory, next.WorkingDirectory, L"WorkingDirectory");
        updateField(_gitBranch, next.GitBranch, L"GitBranch");
        updateField(_gitRepositoryRoot, next.GitRepositoryRoot, L"GitRepositoryRoot");
        updateField(_listeningPortsDisplay, next.ListeningPortsDisplay, L"ListeningPortsDisplay");
        updateField(_listeningPortsTooltip, next.ListeningPortsTooltip, L"ListeningPortsTooltip");
        updateField(_statusText, next.StatusText, L"StatusText");
        updateField(_unreadBadgeText, next.UnreadBadgeText, L"UnreadBadgeText");
        updateField(_latestNotification, next.LatestNotification, L"LatestNotification");
        updateField(_surfaceCountText, next.SurfaceCountText, L"SurfaceCountText");
        updateField(_paneCountBadgeText, next.PaneCountBadgeText, L"PaneCountBadgeText");
        updateField(_switchShortcutHintText, next.SwitchShortcutHintText, L"SwitchShortcutHintText");
        updateField(_treeBranchGlyph, next.TreeBranchGlyph, L"TreeBranchGlyph");
        updateField(_chevronGlyph, next.ChevronGlyph, L"ChevronGlyph");
        updateField(_pinIconGlyph, next.PinIconGlyph, L"PinIconGlyph");
        updateField(_pinActionText, next.PinActionText, L"PinActionText");
        updateField(_pinAutomationName, next.PinAutomationName, L"PinAutomationName");
        updateField(_notificationAutomationName, next.NotificationAutomationName, L"NotificationAutomationName");
        updateField(_notificationHistoryCountText, next.NotificationHistoryCountText, L"NotificationHistoryCountText");
        updateField(_automationName, next.AutomationName, L"AutomationName");

        updateField(_isPinned, next.IsPinned, L"IsPinned");
        updateField(_isSelected, next.IsSelected, L"IsSelected");
        updateField(_isActive, next.IsActive, L"IsActive");
        updateField(_isCollapsed, next.IsCollapsed, L"IsCollapsed");
        updateField(_isRunning, next.IsRunning, L"IsRunning");
        updateField(_hasAttention, next.HasAttention, L"HasAttention");
        if (IsIdle() != wasIdle)
        {
            anyChanged = true;
            PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsIdle" });
        }
        updateField(_showCollapsedUnread, next.ShowCollapsedUnread, L"ShowCollapsedUnread");
        updateField(_hasAgentResume, next.HasAgentResume, L"HasAgentResume");
        updateField(_isVimActive, next.IsVimActive, L"IsVimActive");
        updateField(_isTerminalIcon, next.IsTerminalIcon, L"IsTerminalIcon");
        updateField(_isClaudeIcon, next.IsClaudeIcon, L"IsClaudeIcon");
        updateField(_isCodexIcon, next.IsCodexIcon, L"IsCodexIcon");
        updateField(_isAgyIcon, next.IsAgyIcon, L"IsAgyIcon");

        return anyChanged;
    }

    bool WorkspaceSidebarRow::UpdateFrom(const WorkspaceSidebarRow& next)
    {
        bool anyChanged = false;
        const auto updateField = [&](auto& field, const auto& nextValue, const std::wstring_view propertyName) {
            if (field != nextValue)
            {
                field = nextValue;
                anyChanged = true;
                PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ propertyName });
            }
        };

        const bool wasIdle = IsIdle();

        updateField(_title, next._title, L"Title");
        updateField(_subtitle, next._subtitle, L"Subtitle");
        updateField(_directoryDisplay, next._directoryDisplay, L"DirectoryDisplay");
        updateField(_workingDirectory, next._workingDirectory, L"WorkingDirectory");
        updateField(_gitBranch, next._gitBranch, L"GitBranch");
        updateField(_gitRepositoryRoot, next._gitRepositoryRoot, L"GitRepositoryRoot");
        updateField(_listeningPortsDisplay, next._listeningPortsDisplay, L"ListeningPortsDisplay");
        updateField(_listeningPortsTooltip, next._listeningPortsTooltip, L"ListeningPortsTooltip");
        updateField(_statusText, next._statusText, L"StatusText");
        updateField(_unreadBadgeText, next._unreadBadgeText, L"UnreadBadgeText");
        updateField(_latestNotification, next._latestNotification, L"LatestNotification");
        updateField(_surfaceCountText, next._surfaceCountText, L"SurfaceCountText");
        updateField(_paneCountBadgeText, next._paneCountBadgeText, L"PaneCountBadgeText");
        updateField(_switchShortcutHintText, next._switchShortcutHintText, L"SwitchShortcutHintText");
        updateField(_treeBranchGlyph, next._treeBranchGlyph, L"TreeBranchGlyph");
        updateField(_chevronGlyph, next._chevronGlyph, L"ChevronGlyph");
        updateField(_pinIconGlyph, next._pinIconGlyph, L"PinIconGlyph");
        updateField(_pinActionText, next._pinActionText, L"PinActionText");
        updateField(_pinAutomationName, next._pinAutomationName, L"PinAutomationName");
        updateField(_notificationAutomationName, next._notificationAutomationName, L"NotificationAutomationName");
        updateField(_notificationHistoryCountText, next._notificationHistoryCountText, L"NotificationHistoryCountText");
        updateField(_automationName, next._automationName, L"AutomationName");

        updateField(_isPinned, next._isPinned, L"IsPinned");
        updateField(_isSelected, next._isSelected, L"IsSelected");
        updateField(_isActive, next._isActive, L"IsActive");
        updateField(_isCollapsed, next._isCollapsed, L"IsCollapsed");
        updateField(_isRunning, next._isRunning, L"IsRunning");
        updateField(_hasAttention, next._hasAttention, L"HasAttention");
        if (IsIdle() != wasIdle)
        {
            anyChanged = true;
            PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsIdle" });
        }
        updateField(_showCollapsedUnread, next._showCollapsedUnread, L"ShowCollapsedUnread");
        updateField(_hasAgentResume, next._hasAgentResume, L"HasAgentResume");
        updateField(_isVimActive, next._isVimActive, L"IsVimActive");
        updateField(_isTerminalIcon, next._isTerminalIcon, L"IsTerminalIcon");
        updateField(_isClaudeIcon, next._isClaudeIcon, L"IsClaudeIcon");
        updateField(_isCodexIcon, next._isCodexIcon, L"IsCodexIcon");
        updateField(_isAgyIcon, next._isAgyIcon, L"IsAgyIcon");

        return anyChanged;
    }

    Windows::UI::Xaml::DataTemplate WorkspaceSidebarRowTemplateSelector::SelectTemplateCore(
        const Windows::Foundation::IInspectable& item,
        const Windows::UI::Xaml::DependencyObject& /*container*/)
    {
        return SelectTemplateCore(item);
    }

    Windows::UI::Xaml::DataTemplate WorkspaceSidebarRowTemplateSelector::SelectTemplateCore(
        const Windows::Foundation::IInspectable& item)
    {
        if (const auto row = item.try_as<TerminalApp::WorkspaceSidebarRow>())
        {
            switch (row.Kind())
            {
            case TerminalApp::WorkspaceSidebarRowKind::Surface:
                return SurfaceTemplate();
            case TerminalApp::WorkspaceSidebarRowKind::Pane:
                return PaneTemplate();
            case TerminalApp::WorkspaceSidebarRowKind::Workspace:
            default:
                return WorkspaceTemplate();
            }
        }
        return WorkspaceTemplate();
    }
}
