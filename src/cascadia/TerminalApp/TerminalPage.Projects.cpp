// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TerminalPage.h"
#include "WorkspaceFeatureSet.h"
#include "Tasks/TaskPage.h"
#include "Agents/AgentCatalogPage.h"
#include "RuntimeCrypto.h"
#include "ProtocolValidation.h"
#include <Utils.h>

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::TerminalApp::implementation
{
    using namespace projects;
    using namespace projects::protocol;
    namespace
    {
        std::string projectId(const winrt::guid& id)
        {
            auto value = til::u16u8(winrt::to_hstring(id));
            if (value.size() == 38) value = value.substr(1, 36);
            std::ranges::transform(value, value.begin(), [](char c) { return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c; });
            return value;
        }
    }
    void TerminalPage::SetStartupProjectLayout(const winrt::hstring& layout)
    {
        auto parsed = std::make_shared<ProjectNavigationState>(nullptr);
        const auto json = til::u16u8(layout);
        if (parsed->RestoreLayout(json))
        {
            _startupProjectLayout = json;
            _projectModeWasEnabled = true;
            _SessionRestore().PreparedEmptyWindow = true;
        }
    }
    bool TerminalPage::_ProjectModeEnabled() const noexcept
    { return _projectControls && _tabContent && _workspaceSurfaceNewTabButton && _SidebarController().Enabled && _projects && (_projectModeWasEnabled || _projects->Ready()); }
    void TerminalPage::_InitializeProjectNavigation(std::shared_ptr<tasks::ITaskProjectionSource> source)
    {
        _projectTaskSource = std::move(source);
        _projects = std::make_shared<ProjectNavigationState>(_projectTaskSource ? _projectTaskSource->WorkspaceSource() : nullptr);
        if (!_startupProjectLayout.empty()) static_cast<void>(_projects->RestoreLayout(_startupProjectLayout));
        _projectControls = std::make_unique<ProjectNavigationControls>();
        _projectControls->Toolbar.Visibility(Visibility::Collapsed); _projectControls->Content.Visibility(Visibility::Collapsed);
        TaskPanelHost().Children().Append(_projectControls->Toolbar);
        Grid::SetRow(_projectControls->Content, 1); TaskPanelHost().Children().Append(_projectControls->Content);
        const auto weak = get_weak();
        _projects->SetChangedHandler([weak] { if (const auto self = weak.get()) self->_RenderProjectNavigation(); });
        _projectControls->SetHandler([weak](ProjectNavigationAction action, std::string id) {
            if (const auto self = weak.get()) self->_ProjectNavigationRequested(action, std::move(id));
        });
        _projectRefreshTimer = DispatcherTimer{}; _projectRefreshTimer.Interval(std::chrono::seconds{ 2 });
        _projectRefreshTimer.Tick([weak](const auto&, const auto&) { if (const auto self = weak.get()) self->_RefreshProjects(); });
        _projectRefreshTimer.Start();
    }
    void TerminalPage::_RefreshProjects()
    {
        if (!_ProjectModeEnabled()) return;
        const auto first = !_projectModeWasEnabled; _projectModeWasEnabled = true;
        for (const auto& workspace : _WorkspaceItems())
        {
            const auto id = projectId(workspace.WorkspaceId());
            auto name = til::u16u8(workspace.Title());
            if (name.empty()) name = "Workspace " + id.substr(0, 8);
            _projects->RegisterLegacy(id, name);
            _projects->Views.try_emplace(id, ProjectView::Tasks);
            if (_knownProjectWorkspaces.insert(workspace.WorkspaceId()).second &&
                std::ranges::find(_projects->OpenProjects, id) == _projects->OpenProjects.end())
                _projects->OpenProjects.push_back(id);
        }
        if (_projects->ActiveProjectId.empty() && !_projects->OpenProjects.empty())
            _projects->ActiveProjectId = _projects->OpenProjects.front();
        _projects->Refresh();
        if (!_startupProjectLayout.empty() && _startupState == StartupState::Initialized)
        {
            _startupProjectLayout.clear();
            if (_tabs.Size() == 0) _SetWorkspaceToolPage(WorkspaceToolPage::Tasks);
        }
        if (first) _RenderProjectNavigation();
    }
    bool TerminalPage::_SyncProjectTaskContext()
    {
        if (!_ProjectModeEnabled() || !_taskPage) return false;
        if (const auto project = _projects->Active())
        { winrt::get_self<implementation::TaskPage>(_taskPage)->SetProject(*project); return true; }
        return false;
    }
    void TerminalPage::_RenderProjectNavigation()
    {
        if (!_projectControls || _syncingProjectNavigation) return;
        const auto visible = _ProjectModeEnabled() && _activeWorkspaceToolPage == WorkspaceToolPage::Tasks;
        _projectControls->Toolbar.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (!visible)
        {
            _projectControls->Content.Visibility(Visibility::Collapsed);
            if (_globalAgentPage) _globalAgentPage.Visibility(Visibility::Collapsed);
            return;
        }
        _syncingProjectNavigation = true;
        const auto restore = wil::scope_exit([&] { _syncingProjectNavigation = false; });
        const auto active = _projects->Active();
        _projectControls->Render(*_projects, _projectLibraryVisible);
        const auto overview = _projectLibraryVisible || !active || _projects->CurrentView() == ProjectView::Overview;
        _projectControls->Content.Visibility(overview && !_globalAgentsVisible ? Visibility::Visible : Visibility::Collapsed);
        _taskPage.Visibility(!overview && !_globalAgentsVisible ? Visibility::Visible : Visibility::Collapsed);
        if (_globalAgentPage) _globalAgentPage.Visibility(_globalAgentsVisible ? Visibility::Visible : Visibility::Collapsed);
        _SyncProjectTaskContext();
    }
    void TerminalPage::_SyncActiveProjectFromTab(const TerminalApp::Tab& tab)
    {
        if (!_ProjectModeEnabled() || _syncingProjectNavigation || !tab) return;
        if (_startupState != StartupState::Initialized && !_startupProjectLayout.empty()) return;
        const auto id = projectId(_WorkspaceIdForTab(tab)); if (!IsCanonicalProtocolUuid(id)) return;
        if (id == _projects->ActiveProjectId) return;
        _projectLibraryVisible = false; _globalAgentsVisible = false;
        _projects->Open(id);
    }
    void TerminalPage::_ProjectNavigationRequested(ProjectNavigationAction action, std::string id)
    {
        if (!_ProjectModeEnabled()) return;
        if (action == ProjectNavigationAction::NewProject) { _PickProjectDirectory(false); return; }
        if (action == ProjectNavigationAction::Bind) { _PickProjectDirectory(true); return; }
        if (action == ProjectNavigationAction::CloseProject) { _CloseProjectView(std::move(id)); return; }
        if (action == ProjectNavigationAction::Archive) { _ArchiveProject(std::move(id)); return; }
        if (action == ProjectNavigationAction::Retry) { _projects->Retry(); return; }
        if (action == ProjectNavigationAction::Agents)
        {
            if (_globalAgentsVisible && _globalAgentPage) return;
            if (!_projectTaskSource || !_projectTaskSource->AgentCatalog()) return;
            if (!_globalAgentPage)
            {
                _globalAgentPage = winrt::make<implementation::AgentCatalogPage>();
                Grid::SetRow(_globalAgentPage, 1); TaskPanelHost().Children().Append(_globalAgentPage);
            }
            _globalAgentsVisible = true;
            const auto project = _projects->Active();
            winrt::get_self<implementation::AgentCatalogPage>(_globalAgentPage)->Open(_projectTaskSource->AgentCatalog(), _projects->ActiveProjectId,
                project ? til::u8u16(project->Name) : std::wstring{}, _dialogPresenter.get(), [weak = get_weak()] {
                    if (const auto self = weak.get()) { self->_globalAgentsVisible = false; self->_RenderProjectNavigation(); }
                });
            _globalAgentPage.FindName(L"BackButton").as<Button>().Content(winrt::box_value(RS_(L"Project_Back")));
            _globalAgentPage.FindName(L"WorkspaceTarget").as<TextBlock>().Text(project ? winrt::hstring{ std::wstring{ RS_(L"Project_AgentTarget") } + til::u8u16(project->Name) } : RS_(L"Project_Agents"));
            _globalAgentPage.FindName(L"SetDefaultButton").as<Button>().Content(winrt::box_value(RS_(L"Project_SetDefaultAgent")));
            _globalAgentPage.FindName(L"ClearDefaultButton").as<Button>().Content(winrt::box_value(RS_(L"Project_ClearDefaultAgent")));
            _RenderProjectNavigation(); return;
        }
        _globalAgentsVisible = false;
        if (action == ProjectNavigationAction::Library) { _projectLibraryVisible = true; _RenderProjectNavigation(); return; }
        _projectLibraryVisible = false;
        if (action == ProjectNavigationAction::SelectProject) { _projects->Views[id] = ProjectView::Tasks; _projects->Open(std::move(id)); return; }
        if (action == ProjectNavigationAction::Terminal) { _OpenProjectTerminal(); return; }
        if (action == ProjectNavigationAction::Overview) { _projects->Show(ProjectView::Overview); return; }
        if (action == ProjectNavigationAction::Tasks) { _projects->Show(ProjectView::Tasks); return; }
        if (action == ProjectNavigationAction::Summary) { _SetWorkspaceToolPage(WorkspaceToolPage::Summary); return; }
        const auto project = _projects->Active(); if (!project) return;
        const auto command = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid(); if (!command) return;
        ProjectCatalogCommandRequest request; request.CommandId = *command; request.ProjectId = project->Id; request.ExpectedVersion = project->Version;
        if (action == ProjectNavigationAction::Restore) request.Operation = ProjectCatalogOperation::Restore;
        else if (action == ProjectNavigationAction::Save)
        {
            const auto draft = _projectControls->Draft(); request.Operation = ProjectCatalogOperation::UpdateMetadata;
            request.ExpectedVersion = draft.Version; request.Name = til::u16u8(draft.Name); request.Description = til::u16u8(draft.Description);
        }
        else return;
        _projects->Execute(std::move(request));
    }
    safe_void_coroutine TerminalPage::_PickProjectDirectory(bool bind)
    {
        const auto lifetime = get_strong();
        if (!_ProjectModeEnabled() || _projects->CommandPending || _projects->OutcomeUnknown || !_hostingHwnd) co_return;
        const auto project = _projects->Active();
        const auto target = project ? project->Id : std::string{}; const auto version = project ? project->Version : 0;
        if (bind && !project) co_return;
        const auto folder = co_await OpenFilePicker(*_hostingHwnd, [](auto dialog) {
            DWORD flags{}; THROW_IF_FAILED(dialog->GetOptions(&flags)); THROW_IF_FAILED(dialog->SetOptions(flags | FOS_PICKFOLDERS));
        });
        if (folder.empty()) co_return;
        _projectLibraryVisible = false; _globalAgentsVisible = false;
        const auto command = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid(); const auto newId = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid();
        if (!command || !newId) co_return;
        ProjectCatalogCommandRequest request;
        request.CommandId = *command; request.ProjectId = bind ? target : *newId; request.ExpectedVersion = bind ? version : 0;
        request.Operation = bind ? ProjectCatalogOperation::BindDirectory : ProjectCatalogOperation::Create;
        request.RootPath = til::u16u8(folder);
        if (!bind) request.Name = til::u16u8(std::filesystem::path{ std::wstring_view{ folder } }.filename().wstring());
        _projects->Execute(std::move(request));
    }
    void TerminalPage::_OpenProjectTerminal()
    {
        const auto project = _projects->Active(); if (!project) return;
        const auto id = winrt::guid{ project->Id };
        if (const auto index = _workspaceFeatures->Workspaces().ActiveFlatTabIndex(id))
        { _SetWorkspaceToolPage(WorkspaceToolPage::None); _SelectTab(*index); return; }
        if (!project->Binding || project->Archived) return;
        NewTerminalArgs args; args.StartingDirectory(winrt::hstring{ til::u8u16(project->RootPath) });
        const auto pane = _MakePane(args, nullptr, nullptr, true, id);
        if (pane) _CreateNewTabFromPane(pane, -1, id);
    }
    safe_void_coroutine TerminalPage::_CloseProjectView(std::string id)
    {
        const auto lifetime = get_strong(); if (!_ProjectModeEnabled() || !IsCanonicalProtocolUuid(id)) co_return;
        // This closes a view inside Tasks. Workspace terminals retain their original lifecycle.
        _projects->CloseView(id);
        _RenderProjectNavigation();
    }
    safe_void_coroutine TerminalPage::_ArchiveProject(std::string id)
    {
        const auto lifetime = get_strong(); const auto found = _projects->Catalog.find(id);
        if (found == _projects->Catalog.end()) co_return;
        const auto version = found->second.Version; const auto presenter = _dialogPresenter.get(); if (!presenter) co_return;
        ContentDialog dialog; dialog.Title(winrt::box_value(RS_(L"Project_Archive")));
        dialog.Content(winrt::box_value(RS_(L"Project_ArchiveExplanation")));
        dialog.PrimaryButtonText(RS_(L"TaskBoard_Confirm")); dialog.CloseButtonText(RS_(L"TaskBoard_Cancel")); dialog.DefaultButton(ContentDialogButton::Close);
        if (co_await presenter.ShowDialog(dialog) != ContentDialogResult::Primary) co_return;
        const auto command = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid(); if (!command) co_return;
        _projects->Execute({ *command, ProjectCatalogOperation::Archive, id, version });
    }
}
