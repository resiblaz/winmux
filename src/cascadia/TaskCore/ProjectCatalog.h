// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include "TaskStoreResults.h"
#include <array>

namespace Microsoft::Terminal::TaskCore
{
    inline constexpr size_t MaximumProjectPageSize{ 100 };
    inline constexpr size_t MaximumProjectDescriptionBytes{ 16 * 1024 };

    enum class ProjectBindingState : uint8_t { Unbound, Bound };
    enum class ProjectKind : uint8_t { Directory, Git };
    enum class ProjectCommandKind : uint8_t { RegisterLegacy, Create, UpdateMetadata, BindDirectory, Archive, Restore };

    struct ProjectRecord
    {
        WorkspaceId Id;
        uint64_t Version{};
        std::string Name;
        std::string Description;
        ProjectKind Kind{ ProjectKind::Directory };
        ProjectBindingState Binding{ ProjectBindingState::Unbound };
        std::string RootPath;
        std::string RootIdentity;
        bool Archived{};
        bool Legacy{ true };
        int64_t CreatedAtUnixMilliseconds{};
        int64_t UpdatedAtUnixMilliseconds{};
        std::optional<AgentProfileId> DefaultAgent;
        std::array<uint64_t, 7> StatusCounts{};
        std::array<uint64_t, 7> ColumnRevisions{};
        uint64_t ActiveRuns{};
    };

    struct ProjectQuery
    {
        std::optional<WorkspaceId> Id;
        std::string AfterId;
        bool IncludeArchived{};
        size_t Limit{ 50 };
    };

    struct ProjectQueryResult
    {
        TaskStoreCommandStatus Status{ TaskStoreCommandStatus::StorageError };
        std::vector<ProjectRecord> Projects;
        std::string NextAfterId;
        uint64_t Generation{};
        std::string Error;
        bool Succeeded() const noexcept { return Status == TaskStoreCommandStatus::Succeeded; }
    };

    struct ProjectCommand
    {
        CommandId Command;
        std::string PayloadHash;
        WorkspaceId Id;
        ProjectCommandKind Kind{ ProjectCommandKind::RegisterLegacy };
        uint64_t ExpectedVersion{};
        std::string Name;
        std::string Description;
        ProjectKind DirectoryKind{ ProjectKind::Directory };
        std::string RootPath;
        // Filled by the Runtime after DirectoryIdentity validation, never trusted from the wire.
        std::string RootIdentity;
        int64_t TimestampUnixMilliseconds{};
    };

    struct ProjectCommandResult
    {
        TaskStoreCommandStatus Status{ TaskStoreCommandStatus::StorageError };
        WorkspaceId Id;
        uint64_t Version{};
        uint64_t EventSequence{};
        bool Existing{};
        std::string Error;
        bool Succeeded() const noexcept { return Status == TaskStoreCommandStatus::Succeeded || Status == TaskStoreCommandStatus::Replayed; }
    };

    [[nodiscard]] bool NormalizeProjectCommand(ProjectCommand& command, std::string& error);
}
