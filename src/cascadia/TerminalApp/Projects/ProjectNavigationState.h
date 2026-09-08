// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "IWorkspaceSource.h"
#include <map>

namespace winrt::TerminalApp::implementation::projects
{
    enum class ProjectView : uint8_t { Overview, Tasks, Terminal, Summary };
    class ProjectNavigationState final : public std::enable_shared_from_this<ProjectNavigationState>
    {
    public:
        explicit ProjectNavigationState(std::shared_ptr<IWorkspaceSource> source) : _source{ std::move(source) } {}
        std::map<std::string, protocol::ProjectProjection> Catalog;
        std::vector<std::string> OpenProjects;
        std::string ActiveProjectId;
        std::map<std::string, ProjectView> Views;
        std::wstring Notice;
        bool CommandPending{};
        bool OutcomeUnknown{};
        bool Ready() const noexcept { return _source && _source->Connected() && _source->SupportsProjects(); }
        void SetChangedHandler(std::function<void()> changed) { _changed = std::move(changed); }
        void Refresh();
        void RegisterLegacy(std::string id, std::string name);
        void Open(std::string id);
        void CloseView(std::string_view id);
        void Show(ProjectView view);
        ProjectView CurrentView() const noexcept;
        const protocol::ProjectProjection* Active() const noexcept;
        void Execute(protocol::ProjectCatalogCommandRequest command);
        void Retry();
        std::string SerializeLayout() const;
        bool RestoreLayout(std::string_view json);
    private:
        std::shared_ptr<IWorkspaceSource> _source;
        std::function<void()> _changed;
        std::optional<protocol::ProjectCatalogCommandRequest> _pending;
        std::map<std::string, protocol::ProjectCatalogCommandRequest> _legacyPending;
        std::map<std::string, bool> _legacyRegistered;
        std::map<std::string, protocol::ProjectProjection> _queryCatalog;
        std::string _queryEpoch;
        bool _loading{};
        uint64_t _queryGeneration{};
        uint64_t _generation{};
        std::string _epoch;
        void _QueryPage(std::string after);
        void _SendCommand();
        void _Changed() { if (_changed) _changed(); }
    };
}
