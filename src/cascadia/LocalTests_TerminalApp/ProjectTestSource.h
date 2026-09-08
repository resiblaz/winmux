// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "../TerminalApp/Projects/IWorkspaceSource.h"

namespace TerminalAppLocalTests
{
    namespace project_ui = winrt::TerminalApp::implementation::projects;
    class ProjectTestSource final : public project_ui::IWorkspaceSource
    {
    public:
        std::vector<project_ui::protocol::ProjectProjection> Projects;
        std::vector<project_ui::protocol::ProjectCatalogCommandRequest> Commands;
        bool IsConnected{ true };
        bool Connected() const noexcept override { return IsConnected; }
        bool SupportsProjects() const noexcept override { return true; }
        bool SupportsBoard() const noexcept override { return true; }
        std::string Epoch() const override { return "test-project-runtime"; }
        void QueryProjects(project_ui::protocol::ProjectCatalogQueryRequest, std::function<void(project_ui::WorkspaceReply<project_ui::protocol::ProjectCatalogQueryResult>)> completed) override
        {
            project_ui::WorkspaceReply<project_ui::protocol::ProjectCatalogQueryResult> reply;
            reply.Succeeded = true; reply.Value.Projects = Projects; reply.Value.Generation = 1; completed(std::move(reply));
        }
        void ExecuteProject(project_ui::protocol::ProjectCatalogCommandRequest request, std::function<void(project_ui::WorkspaceReply<project_ui::protocol::ProjectCatalogCommandResult>)> completed) override
        {
            Commands.push_back(request);
            project_ui::WorkspaceReply<project_ui::protocol::ProjectCatalogCommandResult> reply;
            reply.Succeeded = true; reply.Value = { request.CommandId, request.ProjectId, 1, 1, true, false }; completed(std::move(reply));
        }
        void QuerySummaries(project_ui::protocol::TaskSummaryQueryRequest, std::function<void(project_ui::WorkspaceReply<project_ui::protocol::TaskSummaryQueryResult>)> completed) override { completed({}); }
        void ExecuteBoard(project_ui::protocol::TaskBoardCommandRequest, std::function<void(project_ui::WorkspaceReply<project_ui::protocol::TaskBoardCommandResult>)> completed) override { completed({}); }
        void QueryOperation(project_ui::protocol::TaskBoardOperationQueryRequest, std::function<void(project_ui::WorkspaceReply<project_ui::protocol::TaskBoardCommandResult>)> completed) override { completed({}); }
    };
}
