// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeAgentCatalogService.h"
#include "RuntimeCrypto.h"
#include "RuntimeTaskCommandMetadata.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;
    static_assert(MaximumAgentCatalogPageSize == MaximumAgentProfilePageSize);
    static_assert(MaximumAgentInstructionsBytes == MaximumAgentProfileInstructionsBytes);

    RuntimeAgentCatalogResult<AgentCatalogQueryResult> RuntimeAgentCatalogService::Query(const AgentCatalogQueryRequest& request)
    {
        RuntimeAgentCatalogResult<AgentCatalogQueryResult> result;
        if (!Valid()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = "Agent catalog is unavailable"; return result; }
        if (!ValidateAgentCatalogQuery(request, result.Error)) { result.Status = TaskStoreCommandStatus::InvalidCommand; return result; }
        AgentProfileQuery query;
        if (request.AgentId) query.Agent = AgentProfileId{ *request.AgentId };
        if (request.WorkspaceId) query.Workspace = WorkspaceId{ *request.WorkspaceId };
        query.AfterId = request.AfterId; query.IncludeArchived = request.IncludeArchived; query.Limit = request.Limit;
        auto stored = _store->QueryAgentProfiles(query);
        result.Status = stored.Status; result.Error = stored.Error;
        if (!stored.Succeeded()) return result;
        result.Value.Generation = stored.Generation; result.Value.NextAfterId = std::move(stored.NextAfterId);
        if (stored.Default)
            result.Value.Default = WorkspaceAgentDefaultProjection{ stored.Default->Workspace.Value(), stored.Default->Agent ? std::optional{ stored.Default->Agent->Value() } : std::nullopt, stored.Default->Version };
        for (auto& profile : stored.Profiles)
        {
            AgentProfileProjection projection;
            projection.Id = profile.Id.Value(); projection.Version = profile.Version; projection.Revision = profile.Revision;
            projection.Configuration = { std::move(profile.Content.Name), std::move(profile.Content.Description), std::move(profile.Content.Provider), std::move(profile.Content.Model), std::move(profile.Content.Instructions) };
            projection.Archived = profile.Archived; projection.CreatedAtUnixMilliseconds = profile.CreatedAtUnixMilliseconds;
            projection.UpdatedAtUnixMilliseconds = profile.UpdatedAtUnixMilliseconds; projection.ActiveRuns = profile.ActiveRuns;
            for (const auto& workspace : profile.DefaultWorkspaces) projection.DefaultWorkspaces.push_back(workspace.Value());
            result.Value.Profiles.emplace_back(std::move(projection));
        }
        return result;
    }

    RuntimeAgentCatalogResult<AgentCatalogCommandResult> RuntimeAgentCatalogService::Execute(const AgentCatalogCommandRequest& request)
    {
        RuntimeAgentCatalogResult<AgentCatalogCommandResult> result;
        if (!Valid()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = "Agent catalog is unavailable"; return result; }
        const auto encoded = EncodeAgentCatalogCommandRequest(request);
        if (!encoded.Succeeded) { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = encoded.Error; return result; }
        const auto digest = RuntimeSha256Hex(std::span<const uint8_t>{ reinterpret_cast<const uint8_t*>(encoded.Payload.data()), encoded.Payload.size() });
        if (!digest) { result.Error = "Agent command metadata could not be created"; return result; }
        AgentProfileCommand command;
        command.Command = CommandId{ request.CommandId }; command.PayloadHash = *digest;
        command.Kind = static_cast<AgentProfileCommandKind>(request.Operation); command.Agent = AgentProfileId{ request.AgentId };
        if (request.WorkspaceId) command.Workspace = WorkspaceId{ *request.WorkspaceId };
        command.ExpectedVersion = request.ExpectedVersion; command.TimestampUnixMilliseconds = RuntimeUnixMilliseconds();
        if (request.Configuration)
        {
            const auto& c = *request.Configuration;
            command.Content = AgentProfileContent{ c.Name, c.Description, c.Provider, c.Model, c.Instructions };
        }
        const auto stored = _store->ExecuteAgentProfileCommand(command);
        result.Status = stored.Status; result.Error = stored.Error;
        if (stored.Succeeded())
            result.Value = { request.CommandId, request.AgentId, stored.Version, stored.Revision, stored.EventSequence, stored.Status == TaskStoreCommandStatus::Replayed, stored.AffectedDefaults };
        return result;
    }
}
