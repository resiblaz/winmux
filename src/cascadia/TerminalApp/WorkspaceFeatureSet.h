// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "AgentHistoryCoordinator.h"
#include "WorkspaceAutomationController.h"
#include "WorkspaceSnapshotCoordinator.h"
#include "WorkspaceCollection.h"
#include "WorkspaceSessionRestoreState.h"
#include "WorkspaceSidebarController.h"

namespace winrt::TerminalApp::implementation
{
    struct TerminalPage;

    // Composition root for Workspace-specific features owned by a page. It
    // deliberately contains no feature logic of its own.
    class WorkspaceFeatureSet final
    {
    public:
        explicit WorkspaceFeatureSet(TerminalPage& page) noexcept;
        ~WorkspaceFeatureSet();

        WorkspaceFeatureSet(const WorkspaceFeatureSet&) = delete;
        WorkspaceFeatureSet& operator=(const WorkspaceFeatureSet&) = delete;

        WorkspaceAutomationController& Automation() noexcept;
        AgentHistoryCoordinator& History() noexcept;
        WorkspaceSnapshotCoordinator& Snapshots() noexcept;
        WorkspaceCollection& Workspaces() noexcept;
        const WorkspaceCollection& Workspaces() const noexcept;
        WorkspaceSessionRestoreState& SessionRestore() noexcept;
        const WorkspaceSessionRestoreState& SessionRestore() const noexcept;
        WorkspaceSidebarController& Sidebar() noexcept;
        const WorkspaceSidebarController& Sidebar() const noexcept;
        void Stop() noexcept;

    private:
        WorkspaceCollection _workspaces;
        WorkspaceSessionRestoreState _sessionRestore;
        WorkspaceSidebarController _sidebar;
        WorkspaceAutomationController _automation;
        AgentHistoryCoordinator _history;
        WorkspaceSnapshotCoordinator _snapshots;
    };
}
