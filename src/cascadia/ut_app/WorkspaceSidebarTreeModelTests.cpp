// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "../TerminalApp/WorkspaceSidebarTreeModel.h"

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace winrt::TerminalApp::implementation;

namespace TerminalAppUnitTests
{
    class WorkspaceSidebarTreeModelTests
    {
        BEGIN_TEST_CLASS(WorkspaceSidebarTreeModelTests)
        END_TEST_CLASS()

        TEST_METHOD(ProjectsThreeLevelTreeWhenSurfaceHasMultiplePanes);
        TEST_METHOD(AutoAccordionExpandsOnlySelectedWorkspace);
        TEST_METHOD(CollapseAndExpandAllPreserveManualStateAcrossSearch);
        TEST_METHOD(StatusFiltersAndCountsMatchAttentionAndRunning);
        TEST_METHOD(DetectsTerminalClaudeCodexAndAgyIcons);
        TEST_METHOD(PruneCollapsedRemovesClosedWorkspaces);

    private:
        static winrt::guid _makeGuid(uint32_t seed)
        {
            winrt::guid id{};
            id.Data1 = seed;
            id.Data2 = 0x1234;
            id.Data3 = 0x4567;
            id.Data4[0] = 0x89;
            return id;
        }
    };

    void WorkspaceSidebarTreeModelTests::ProjectsThreeLevelTreeWhenSurfaceHasMultiplePanes()
    {
        WorkspaceSidebarTreeModel model;
        std::vector<SidebarWorkspaceInput> inputs;

        SidebarWorkspaceInput ws{};
        ws.WorkspaceId = _makeGuid(1);
        ws.Title = L"Winmux Core";
        ws.Directory = L"F:\\codes\\win_terminal";
        ws.GitBranch = L"develop";
        ws.IsSelected = true;

        SidebarSurfaceInput sfMulti{};
        sfMulti.SurfaceId = _makeGuid(101);
        sfMulti.Title = L"codex · sidebar";
        sfMulti.IsActive = true;
        sfMulti.IsRunning = true;
        sfMulti.Panes.push_back(SidebarPaneInput{ 0, L"codex", true, false, true });
        sfMulti.Panes.push_back(SidebarPaneInput{ 1, L"pwsh build", false, true, false });
        ws.Surfaces.push_back(std::move(sfMulti));

        SidebarSurfaceInput sfSingle{};
        sfSingle.SurfaceId = _makeGuid(102);
        sfSingle.Title = L"git diff";
        sfSingle.Panes.push_back(SidebarPaneInput{ 0, L"pwsh", true, false, false });
        ws.Surfaces.push_back(std::move(sfSingle));

        model.SetSelectedWorkspace(ws.WorkspaceId);
        inputs.push_back(std::move(ws));

        const auto projection = model.Project(inputs);
        VERIFY_ARE_EQUAL(2u, projection.TotalSurfaceCount);
        VERIFY_ARE_EQUAL(1u, projection.AttentionSurfaceCount);
        VERIFY_ARE_EQUAL(1u, projection.RunningSurfaceCount);
        VERIFY_IS_TRUE(projection.AnyWorkspaceExpanded);
        VERIFY_IS_FALSE(projection.IsFiltered);

        // Expect: Workspace, Surface1, Pane0, Pane1, Surface2 (single-pane surface emits no 3rd level)
        VERIFY_ARE_EQUAL(size_t{ 5 }, projection.Nodes.size());
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarNodeKind::Workspace), static_cast<int>(projection.Nodes[0].Kind));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarNodeKind::Surface), static_cast<int>(projection.Nodes[1].Kind));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Codex), static_cast<int>(projection.Nodes[1].Icon));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarNodeKind::Pane), static_cast<int>(projection.Nodes[2].Kind));
        VERIFY_ARE_EQUAL(0u, projection.Nodes[2].PaneId);
        VERIFY_IS_FALSE(projection.Nodes[2].IsLastChild);
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Codex), static_cast<int>(projection.Nodes[2].Icon));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarNodeKind::Pane), static_cast<int>(projection.Nodes[3].Kind));
        VERIFY_ARE_EQUAL(1u, projection.Nodes[3].PaneId);
        VERIFY_IS_TRUE(projection.Nodes[3].IsLastChild);
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarStatusKind::Attention), static_cast<int>(projection.Nodes[3].Status));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Terminal), static_cast<int>(projection.Nodes[3].Icon));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarNodeKind::Surface), static_cast<int>(projection.Nodes[4].Kind));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Terminal), static_cast<int>(projection.Nodes[4].Icon));
    }

    void WorkspaceSidebarTreeModelTests::AutoAccordionExpandsOnlySelectedWorkspace()
    {
        WorkspaceSidebarTreeModel model;
        const auto ws1Id = _makeGuid(1);
        const auto ws2Id = _makeGuid(2);

        std::vector<SidebarWorkspaceInput> inputs(2);
        inputs[0].WorkspaceId = ws1Id;
        inputs[0].Title = L"Workspace 1";
        inputs[0].IsSelected = true;
        inputs[0].Surfaces.push_back(SidebarSurfaceInput{ _makeGuid(11), L"PowerShell", L"", 0, true });

        inputs[1].WorkspaceId = ws2Id;
        inputs[1].Title = L"Workspace 2";
        inputs[1].IsSelected = false;
        inputs[1].UnreadCount = 2;
        inputs[1].Surfaces.push_back(SidebarSurfaceInput{ _makeGuid(21), L"claude", L"ready", 2, false, false, true });

        model.SetSelectedWorkspace(ws1Id);
        auto proj = model.Project(inputs);
        // ws1 expanded (+1 surface), ws2 collapsed (+collapsed unread badge) -> 3 nodes total.
        VERIFY_ARE_EQUAL(size_t{ 3 }, proj.Nodes.size());
        VERIFY_IS_FALSE(proj.Nodes[0].IsCollapsed);
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarNodeKind::Surface), static_cast<int>(proj.Nodes[1].Kind));
        VERIFY_ARE_EQUAL(ws2Id, proj.Nodes[2].WorkspaceId);
        VERIFY_IS_TRUE(proj.Nodes[2].IsCollapsed);
        VERIFY_IS_TRUE(proj.Nodes[2].ShowCollapsedUnread);

        // Switching selection to ws2 auto-collapses ws1 and auto-expands ws2.
        inputs[0].IsSelected = false;
        inputs[1].IsSelected = true;
        model.SetSelectedWorkspace(ws2Id);
        proj = model.Project(inputs);
        VERIFY_ARE_EQUAL(size_t{ 3 }, proj.Nodes.size());
        VERIFY_ARE_EQUAL(ws1Id, proj.Nodes[0].WorkspaceId);
        VERIFY_IS_TRUE(proj.Nodes[0].IsCollapsed);
        VERIFY_ARE_EQUAL(ws2Id, proj.Nodes[1].WorkspaceId);
        VERIFY_IS_FALSE(proj.Nodes[1].IsCollapsed);
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarNodeKind::Surface), static_cast<int>(proj.Nodes[2].Kind));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Claude), static_cast<int>(proj.Nodes[2].Icon));
    }

    void WorkspaceSidebarTreeModelTests::CollapseAndExpandAllPreserveManualStateAcrossSearch()
    {
        WorkspaceSidebarTreeModel model;
        const auto ws1Id = _makeGuid(1);
        const auto ws2Id = _makeGuid(2);

        std::vector<SidebarWorkspaceInput> inputs(2);
        inputs[0].WorkspaceId = ws1Id;
        inputs[0].Title = L"Backend API";
        inputs[0].IsSelected = false;
        inputs[0].UnreadCount = 2;
        inputs[0].Surfaces.push_back(SidebarSurfaceInput{
            _makeGuid(11), L"claude · auth", L"awaiting approval", 2, false, false, true, false, SidebarIconKind::Terminal, { SidebarPaneInput{ 0, L"claude", true, true, false } }
        });

        inputs[1].WorkspaceId = ws2Id;
        inputs[1].Title = L"SSH Ops";
        inputs[1].IsSelected = true;
        inputs[1].Surfaces.push_back(SidebarSurfaceInput{
            _makeGuid(21), L"ssh prod-gateway", L"", 0, true, false, false, false, SidebarIconKind::Terminal, { SidebarPaneInput{ 0, L"ssh", true, false, false } }
        });

        model.SetSelectedWorkspace(ws2Id);

        // By default ws1 (unselected) is collapsed and ws2 (selected) is expanded.
        auto proj = model.Project(inputs);
        VERIFY_ARE_EQUAL(size_t{ 3 }, proj.Nodes.size());
        VERIFY_IS_TRUE(proj.Nodes[0].IsCollapsed);
        VERIFY_IS_TRUE(proj.Nodes[0].ShowCollapsedUnread);
        VERIFY_IS_FALSE(proj.Nodes[1].IsCollapsed);

        // Search matching ws1's child surface temporarily expands ws1 without erasing accordion state.
        model.SetSearchQuery(L"AUTH");
        proj = model.Project(inputs);
        VERIFY_IS_TRUE(proj.IsFiltered);
        VERIFY_ARE_EQUAL(size_t{ 2 }, proj.Nodes.size());
        VERIFY_IS_FALSE(proj.Nodes[0].IsCollapsed);
        VERIFY_ARE_EQUAL(ws1Id, proj.Nodes[0].WorkspaceId);
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarNodeKind::Surface), static_cast<int>(proj.Nodes[1].Kind));

        // Clearing search restores ws1 collapsed and ws2 expanded.
        model.SetSearchQuery(L"");
        proj = model.Project(inputs);
        VERIFY_IS_FALSE(proj.IsFiltered);
        VERIFY_IS_TRUE(model.IsWorkspaceCollapsed(ws1Id));
        VERIFY_IS_FALSE(model.IsWorkspaceCollapsed(ws2Id));
        VERIFY_ARE_EQUAL(size_t{ 3 }, proj.Nodes.size());

        // ToggleCollapseAll collapses all since ws2 is expanded, then expands all on second call.
        VERIFY_IS_TRUE(model.ToggleCollapseAll({ ws1Id, ws2Id }));
        proj = model.Project(inputs);
        VERIFY_IS_FALSE(proj.AnyWorkspaceExpanded);
        VERIFY_ARE_EQUAL(size_t{ 2 }, proj.Nodes.size());

        VERIFY_IS_FALSE(model.ToggleCollapseAll({ ws1Id, ws2Id }));
        proj = model.Project(inputs);
        VERIFY_IS_TRUE(proj.AnyWorkspaceExpanded);
        VERIFY_ARE_EQUAL(size_t{ 4 }, proj.Nodes.size());
    }

    void WorkspaceSidebarTreeModelTests::StatusFiltersAndCountsMatchAttentionAndRunning()
    {
        WorkspaceSidebarTreeModel model;
        std::vector<SidebarWorkspaceInput> inputs(2);
        inputs[0].WorkspaceId = _makeGuid(10);
        inputs[0].Title = L"Alpha";
        inputs[0].IsSelected = true;
        inputs[0].Surfaces.push_back(SidebarSurfaceInput{
            _makeGuid(101), L"dev server", L"", 0, false, true, false, false, SidebarIconKind::Terminal, {}
        });
        inputs[0].Surfaces.push_back(SidebarSurfaceInput{
            _makeGuid(102), L"idle shell", L"", 0, false, false, false, false, SidebarIconKind::Terminal, {}
        });

        inputs[1].WorkspaceId = _makeGuid(20);
        inputs[1].Title = L"Beta";
        inputs[1].Surfaces.push_back(SidebarSurfaceInput{
            _makeGuid(201), L"agent task", L"done", 1, false, false, true, false, SidebarIconKind::Terminal, {}
        });

        model.SetSelectedWorkspace(inputs[0].WorkspaceId);
        model.SetFilterMode(SidebarFilterMode::Running);
        auto proj = model.Project(inputs);
        VERIFY_ARE_EQUAL(3u, proj.TotalSurfaceCount);
        VERIFY_ARE_EQUAL(1u, proj.AttentionSurfaceCount);
        VERIFY_ARE_EQUAL(1u, proj.RunningSurfaceCount);
        VERIFY_ARE_EQUAL(size_t{ 2 }, proj.Nodes.size());
        VERIFY_ARE_EQUAL(_makeGuid(10), proj.Nodes[0].WorkspaceId);
        VERIFY_ARE_EQUAL(_makeGuid(101), proj.Nodes[1].SurfaceId);

        model.SetFilterMode(SidebarFilterMode::Attention);
        proj = model.Project(inputs);
        VERIFY_ARE_EQUAL(size_t{ 2 }, proj.Nodes.size());
        VERIFY_ARE_EQUAL(_makeGuid(20), proj.Nodes[0].WorkspaceId);
        VERIFY_ARE_EQUAL(_makeGuid(201), proj.Nodes[1].SurfaceId);
    }

    void WorkspaceSidebarTreeModelTests::DetectsTerminalClaudeCodexAndAgyIcons()
    {
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Terminal), static_cast<int>(WorkspaceSidebarTreeModel::DetectIconKind(L"PowerShell")));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Terminal), static_cast<int>(WorkspaceSidebarTreeModel::DetectIconKind(L"strategy-notes")));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Claude), static_cast<int>(WorkspaceSidebarTreeModel::DetectIconKind(L"Claude Code")));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Claude), static_cast<int>(WorkspaceSidebarTreeModel::DetectIconKind(L"claude.exe")));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Codex), static_cast<int>(WorkspaceSidebarTreeModel::DetectIconKind(L"codex · refactor")));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Codex), static_cast<int>(WorkspaceSidebarTreeModel::DetectIconKind(L"codex.exe")));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Agy), static_cast<int>(WorkspaceSidebarTreeModel::DetectIconKind(L"agy")));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Agy), static_cast<int>(WorkspaceSidebarTreeModel::DetectIconKind(L"agy.exe")));
        VERIFY_ARE_EQUAL(static_cast<int>(SidebarIconKind::Agy), static_cast<int>(WorkspaceSidebarTreeModel::DetectIconKind(L"Antigravity CLI")));
    }

    void WorkspaceSidebarTreeModelTests::PruneCollapsedRemovesClosedWorkspaces()
    {
        WorkspaceSidebarTreeModel model;
        const auto keepId = _makeGuid(1);
        const auto closedId = _makeGuid(2);
        model.SetSelectedWorkspace(keepId);
        model.SetWorkspaceCollapsed(keepId, true, true);
        model.SetWorkspaceCollapsed(closedId, false, false);

        model.PruneCollapsed({ keepId });
        VERIFY_IS_TRUE(model.IsWorkspaceCollapsed(keepId, true));
        VERIFY_IS_TRUE(model.IsWorkspaceCollapsed(closedId, false));
    }
}
