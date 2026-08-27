// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceFeatureSet.h"

#include "TerminalPage.h"

namespace winrt::TerminalApp::implementation
{
    WorkspaceFeatureSet::WorkspaceFeatureSet(TerminalPage& page) noexcept :
        _automation{ page },
        _history{ page },
        _snapshots{ page }
    {
    }

    WorkspaceFeatureSet::~WorkspaceFeatureSet()
    {
        Stop();
    }

    WorkspaceAutomationController& WorkspaceFeatureSet::Automation() noexcept
    {
        return _automation;
    }

    AgentHistoryCoordinator& WorkspaceFeatureSet::History() noexcept
    {
        return _history;
    }

    WorkspaceSnapshotCoordinator& WorkspaceFeatureSet::Snapshots() noexcept
    {
        return _snapshots;
    }

    WorkspaceCollection& WorkspaceFeatureSet::Workspaces() noexcept
    {
        return _workspaces;
    }

    const WorkspaceCollection& WorkspaceFeatureSet::Workspaces() const noexcept
    {
        return _workspaces;
    }

    WorkspaceSessionRestoreState& WorkspaceFeatureSet::SessionRestore() noexcept
    {
        return _sessionRestore;
    }

    const WorkspaceSessionRestoreState& WorkspaceFeatureSet::SessionRestore() const noexcept
    {
        return _sessionRestore;
    }

    WorkspaceSidebarController& WorkspaceFeatureSet::Sidebar() noexcept
    {
        return _sidebar;
    }

    const WorkspaceSidebarController& WorkspaceFeatureSet::Sidebar() const noexcept
    {
        return _sidebar;
    }

    void WorkspaceFeatureSet::Stop() noexcept
    {
        _sidebar.Stop();
        _snapshots.Stop();
        _history.Stop();
        _automation.Stop();
    }
}
