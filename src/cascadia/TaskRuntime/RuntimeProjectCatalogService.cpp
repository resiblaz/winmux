// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "RuntimeProjectCatalogService.h"
#include "RuntimeCrypto.h"
#include "RuntimeTaskCommandMetadata.h"
#include "DirectoryIdentity.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;
    using namespace Microsoft::Terminal::TaskExecution;
    static_assert(MaximumProjectCatalogPageSize == MaximumProjectPageSize);

    RuntimeWorkspaceResult<ProjectCatalogQueryResult> RuntimeProjectCatalogService::Query(const ProjectCatalogQueryRequest& request)
    {
        RuntimeWorkspaceResult<ProjectCatalogQueryResult> result;
        if (!Valid()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = "Project catalog is unavailable"; return result; }
        const auto checked = EncodeProjectCatalogQueryRequest(request);
        if (!checked.Succeeded) { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = checked.Error; return result; }
        ProjectQuery query;
        if (request.ProjectId) query.Id = WorkspaceId{ *request.ProjectId };
        query.AfterId = request.AfterId; query.IncludeArchived = request.IncludeArchived; query.Limit = request.Limit;
        const auto stored = _store->QueryProjects(query);
        result.Status = stored.Status; result.Error = stored.Error;
        if (!stored.Succeeded()) return result;
        result.Value.Generation = stored.Generation; result.Value.NextAfterId = stored.NextAfterId;
        for (const auto& project : stored.Projects)
        {
            ProjectProjection item;
            item.Id = project.Id.Value(); item.Version = project.Version; item.Name = project.Name; item.Description = project.Description;
            item.Kind = static_cast<uint8_t>(project.Kind); item.Binding = static_cast<uint8_t>(project.Binding); item.RootPath = project.RootPath;
            item.Archived = project.Archived; item.Legacy = project.Legacy;
            item.CreatedAtUnixMilliseconds = project.CreatedAtUnixMilliseconds; item.UpdatedAtUnixMilliseconds = project.UpdatedAtUnixMilliseconds;
            if (project.DefaultAgent) item.DefaultAgentId = project.DefaultAgent->Value();
            item.StatusCounts = project.StatusCounts; item.ColumnRevisions = project.ColumnRevisions; item.ActiveRuns = project.ActiveRuns;
            result.Value.Projects.push_back(std::move(item));
        }
        while (result.Value.Projects.size() > 1 && !EncodeProjectCatalogQueryResult(result.Value).Succeeded)
        {
            result.Value.Projects.resize((result.Value.Projects.size() + 1) / 2);
            result.Value.NextAfterId = result.Value.Projects.back().Id;
        }
        return result;
    }

    RuntimeWorkspaceResult<ProjectCatalogCommandResult> RuntimeProjectCatalogService::Execute(const ProjectCatalogCommandRequest& request)
    {
        RuntimeWorkspaceResult<ProjectCatalogCommandResult> result;
        if (!Valid()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = "Project catalog is unavailable"; return result; }
        const auto encoded = EncodeProjectCatalogCommandRequest(request);
        if (!encoded.Succeeded) { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = encoded.Error; return result; }
        const auto digest = RuntimeSha256Hex(std::span<const uint8_t>{ reinterpret_cast<const uint8_t*>(encoded.Payload.data()), encoded.Payload.size() });
        if (!digest) { result.Error = "Project command metadata could not be created"; return result; }
        // A reconnect must recover a committed command even if its directory has since moved.
        auto stored = _store->QueryProjectCommandReceipt(CommandId{ request.CommandId }, *digest);
        if (stored.Status == TaskStoreCommandStatus::NotFound)
        {
            ProjectCommand command;
            command.Command = CommandId{ request.CommandId }; command.PayloadHash = *digest;
            command.Id = WorkspaceId{ request.ProjectId }; command.Kind = static_cast<ProjectCommandKind>(request.Operation);
            command.ExpectedVersion = request.ExpectedVersion; command.Name = request.Name; command.Description = request.Description;
            command.TimestampUnixMilliseconds = RuntimeUnixMilliseconds();
            if (command.Kind == ProjectCommandKind::Create || command.Kind == ProjectCommandKind::BindDirectory)
            {
                const std::u8string input{ reinterpret_cast<const char8_t*>(request.RootPath.data()), request.RootPath.size() };
                const auto identity = ResolveCanonicalDirectoryIdentity(std::filesystem::path{ input }, DirectoryIdentityPolicy::Direct);
                if (!identity.Succeeded()) { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = identity.Error; return result; }
                // Compare path components through stable directory identities, including aliases.
                const auto managed = ResolveCanonicalDirectoryIdentity(_runtimeRoot / L"task-worktrees", DirectoryIdentityPolicy::Direct);
                if (!managed.Succeeded() && managed.Status != DirectoryIdentityStatus::MissingPath)
                { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = "Managed Task directory could not be verified"; return result; }
                if (managed.Succeeded())
                {
                    auto parent = identity.Identity->CanonicalPath();
                    while (parent != parent.root_path() && !parent.empty())
                    {
                        const auto ancestor = ResolveCanonicalDirectoryIdentity(parent, DirectoryIdentityPolicy::Direct);
                        if (!ancestor.Succeeded()) { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = "Project directory ancestry changed"; return result; }
                        if (ancestor.Identity->Digest() == managed.Identity->Digest())
                        { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = "A managed Task Worktree cannot be a project root"; return result; }
                        parent = parent.parent_path();
                    }
                }
                const auto path = identity.Identity->CanonicalPath().u8string();
                command.RootPath.assign(reinterpret_cast<const char*>(path.data()), path.size());
                command.RootIdentity = identity.Identity->Digest();
                std::error_code error;
                command.DirectoryKind = std::filesystem::exists(identity.Identity->CanonicalPath() / L".git", error) && !error ? ProjectKind::Git : ProjectKind::Directory;
            }
            stored = _store->ExecuteProjectCommand(command);
        }
        result.Status = stored.Status; result.Error = stored.Error;
        if (stored.Succeeded())
            result.Value = { request.CommandId, stored.Id.Value(), stored.Version, stored.EventSequence, stored.Existing, stored.Status == TaskStoreCommandStatus::Replayed };
        return result;
    }
}
