// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "ProjectNavigationState.h"
#include "RuntimeCrypto.h"

namespace winrt::TerminalApp::implementation::projects
{
    using namespace protocol;
    void ProjectNavigationState::Refresh()
    {
        if (!Ready())
        {
            if (!CommandPending && !OutcomeUnknown && Notice != RS_(L"Project_Offline"))
            { Notice = RS_(L"Project_Offline"); _Changed(); }
            return;
        }
        if (_loading) return;
        _loading = true;
        std::vector<std::pair<std::string, std::string>> registrations;
        for (const auto& [id, command] : _legacyPending) registrations.emplace_back(id, command.Name);
        for (const auto& [id, name] : registrations) RegisterLegacy(id, name);
        _queryCatalog.clear(); _queryEpoch = _source->Epoch(); _queryGeneration = 0; _QueryPage({});
    }
    void ProjectNavigationState::_QueryPage(std::string after)
    {
        ProjectCatalogQueryRequest query; query.IncludeArchived = true; query.Limit = 100; query.AfterId = std::move(after);
        const auto first = query.AfterId.empty(); const auto weak = weak_from_this();
        _source->QueryProjects(std::move(query), [weak, first](WorkspaceReply<ProjectCatalogQueryResult> reply) {
            const auto self = weak.lock(); if (!self) return;
            if (!reply.Succeeded || self->_queryEpoch != self->_source->Epoch())
            { self->_loading = false; return; }
            if (first) self->_queryGeneration = reply.Value.Generation;
            else if (self->_queryGeneration != reply.Value.Generation)
            { self->_loading = false; return; }
            for (auto& project : reply.Value.Projects)
            { const auto id = project.Id; self->_queryCatalog.emplace(id, std::move(project)); }
            if (!reply.Value.NextAfterId.empty()) { self->_QueryPage(reply.Value.NextAfterId); return; }
            self->_loading = false;
            const auto wasOffline = self->Notice == RS_(L"Project_Offline");
            if (wasOffline) self->Notice.clear();
            const auto changed = wasOffline || self->_epoch != self->_queryEpoch || self->_generation != self->_queryGeneration || self->Catalog.empty();
            self->Catalog = std::move(self->_queryCatalog); self->_generation = self->_queryGeneration; self->_epoch = self->_queryEpoch;
            if (changed) self->_Changed();
        });
    }
    void ProjectNavigationState::RegisterLegacy(std::string id, std::string name)
    {
        if (_legacyRegistered.contains(id)) return;
        auto found = _legacyPending.find(id);
        if (found == _legacyPending.end())
        {
            const auto command = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid(); if (!command) return;
            found = _legacyPending.emplace(id, ProjectCatalogCommandRequest{ *command, ProjectCatalogOperation::RegisterLegacy, id, 0, std::move(name) }).first;
        }
        if (!Ready()) return;
        _legacyRegistered[id] = false;
        const auto weak = weak_from_this();
        _source->ExecuteProject(found->second, [weak, id](WorkspaceReply<ProjectCatalogCommandResult> reply) {
            const auto self = weak.lock(); if (!self) return;
            if (reply.Succeeded) { self->_legacyRegistered[id] = true; self->_legacyPending.erase(id); self->Refresh(); }
            else self->_legacyRegistered.erase(id);
        });
    }
    void ProjectNavigationState::Open(std::string id)
    {
        if (std::ranges::find(OpenProjects, id) == OpenProjects.end()) OpenProjects.push_back(id);
        ActiveProjectId = std::move(id); Views.try_emplace(ActiveProjectId, ProjectView::Tasks); _Changed();
    }
    void ProjectNavigationState::CloseView(std::string_view id)
    {
        const auto found = std::ranges::find(OpenProjects, id);
        if (found == OpenProjects.end()) return;
        const auto index = static_cast<size_t>(found - OpenProjects.begin());
        OpenProjects.erase(found);
        if (ActiveProjectId == id)
            ActiveProjectId = OpenProjects.empty() ? std::string{} : OpenProjects[(std::min)(index, OpenProjects.size() - 1)];
        _Changed();
    }
    void ProjectNavigationState::Show(ProjectView view)
    { if (!ActiveProjectId.empty()) Views[ActiveProjectId] = view; _Changed(); }
    ProjectView ProjectNavigationState::CurrentView() const noexcept
    { const auto found = Views.find(ActiveProjectId); return found == Views.end() ? ProjectView::Overview : found->second; }
    const ProjectProjection* ProjectNavigationState::Active() const noexcept
    { const auto found = Catalog.find(ActiveProjectId); return found == Catalog.end() ? nullptr : &found->second; }
    void ProjectNavigationState::Execute(ProjectCatalogCommandRequest command)
    {
        if (!Ready()) { Notice = RS_(L"Project_Offline"); _Changed(); return; }
        if (_pending) return;
        _pending = std::move(command); _SendCommand();
    }
    void ProjectNavigationState::Retry()
    { if (_pending && OutcomeUnknown && !CommandPending) _SendCommand(); }
    void ProjectNavigationState::_SendCommand()
    {
        if (!_pending || !Ready()) return;
        CommandPending = true; Notice = RS_(L"Project_Saving"); _Changed();
        const auto weak = weak_from_this();
        _source->ExecuteProject(*_pending, [weak](WorkspaceReply<ProjectCatalogCommandResult> reply) {
            const auto self = weak.lock(); if (!self || !self->_pending) return;
            const auto operation = self->_pending->Operation;
            self->CommandPending = false; self->OutcomeUnknown = reply.OutcomeUnknown;
            self->Notice = reply.Succeeded ? std::wstring{ RS_(L"Project_Saved") } : til::u8u16(reply.Error);
            if (!reply.OutcomeUnknown) self->_pending.reset();
            if (reply.Succeeded)
            {
                if (operation == ProjectCatalogOperation::Create || operation == ProjectCatalogOperation::Restore) self->Open(reply.Value.ProjectId);
                if (operation == ProjectCatalogOperation::Archive) self->CloseView(reply.Value.ProjectId);
                self->Refresh();
            }
            self->_Changed();
        });
    }
}
