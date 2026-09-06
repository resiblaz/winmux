// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreResults.h"
#include "TaskStoreCommandTypes.h"

namespace Microsoft::Terminal::TaskCore
{
    inline constexpr size_t MaximumAgentProfileNameScalars{ 80 };
    inline constexpr size_t MaximumAgentProfileDescriptionBytes{ 1024 };
    inline constexpr size_t MaximumAgentProfileInstructionsBytes{ 32 * 1024 };
    inline constexpr size_t MaximumAgentProfileModelBytes{ 256 };
    inline constexpr size_t MaximumAgentProfilePageSize{ 100 };

    struct AgentProfileContent
    {
        std::string Name;
        std::string Description;
        std::string Provider{ "codex" };
        std::string Model;
        std::string Instructions;
    };

    struct AgentProfileSelection
    {
        AgentProfileId Id;
        uint64_t Revision{};
    };

    struct AgentProfileRecord
    {
        AgentProfileId Id;
        uint64_t Version{};
        uint64_t Revision{};
        AgentProfileContent Content;
        bool Archived{};
        int64_t CreatedAtUnixMilliseconds{};
        int64_t UpdatedAtUnixMilliseconds{};
        uint32_t ActiveRuns{};
        std::vector<WorkspaceId> DefaultWorkspaces;
    };

    struct WorkspaceAgentDefault
    {
        WorkspaceId Workspace;
        std::optional<AgentProfileId> Agent;
        uint64_t Version{};
    };

    struct AgentProfileQuery
    {
        std::optional<AgentProfileId> Agent;
        std::optional<WorkspaceId> Workspace;
        std::string AfterId;
        bool IncludeArchived{};
        size_t Limit{ 50 };
    };

    struct AgentProfileQueryResult
    {
        TaskStoreCommandStatus Status{ TaskStoreCommandStatus::StorageError };
        std::vector<AgentProfileRecord> Profiles;
        std::optional<WorkspaceAgentDefault> Default;
        std::string NextAfterId;
        uint64_t Generation{};
        std::string Error;
        bool Succeeded() const noexcept { return Status == TaskStoreCommandStatus::Succeeded; }
    };

    enum class AgentProfileCommandKind : uint8_t { Create, Update, Archive, Restore, SetWorkspaceDefault };

    struct AgentProfileCommand
    {
        CommandId Command;
        std::string PayloadHash;
        AgentProfileCommandKind Kind{ AgentProfileCommandKind::Create };
        AgentProfileId Agent;
        std::optional<WorkspaceId> Workspace;
        uint64_t ExpectedVersion{};
        std::optional<AgentProfileContent> Content;
        int64_t TimestampUnixMilliseconds{};
    };

    struct AgentProfileCommandResult
    {
        TaskStoreCommandStatus Status{ TaskStoreCommandStatus::StorageError };
        uint64_t Version{};
        uint64_t Revision{};
        uint64_t EventSequence{};
        uint32_t AffectedDefaults{};
        std::string Error;
        bool Succeeded() const noexcept { return Status == TaskStoreCommandStatus::Succeeded || Status == TaskStoreCommandStatus::Replayed; }
    };

    struct UpdateTaskExecutionAgentCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        ExecutionContextId Context;
        std::optional<AgentProfileSelection> Agent;
        std::string Provider{ "codex" };
        std::string Model;
        std::string Instructions;
    };

    // Trims the display name while preserving instructions exactly as authored.
    [[nodiscard]] bool NormalizeAgentProfileContent(AgentProfileContent& content, std::string& error);
}
