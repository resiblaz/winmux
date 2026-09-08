// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "ProtocolRpcJson.h"
#include <array>
#include <vector>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr size_t MaximumProjectCatalogPageSize{ 100 };
    enum class ProjectCatalogOperation : uint8_t { RegisterLegacy, Create, UpdateMetadata, BindDirectory, Archive, Restore };
    struct ProjectProjection
    {
        std::string Id;
        uint64_t Version{};
        std::string Name;
        std::string Description;
        uint8_t Kind{};
        uint8_t Binding{};
        std::string RootPath;
        bool Archived{};
        bool Legacy{};
        int64_t CreatedAtUnixMilliseconds{};
        int64_t UpdatedAtUnixMilliseconds{};
        std::optional<std::string> DefaultAgentId;
        std::array<uint64_t, 7> StatusCounts{};
        std::array<uint64_t, 7> ColumnRevisions{};
        uint64_t ActiveRuns{};
    };
    struct ProjectCatalogQueryRequest
    {
        std::optional<std::string> ProjectId;
        std::string AfterId;
        bool IncludeArchived{};
        size_t Limit{ 50 };
    };
    struct ProjectCatalogQueryResult
    {
        std::vector<ProjectProjection> Projects;
        std::string NextAfterId;
        uint64_t Generation{};
    };
    struct ProjectCatalogCommandRequest
    {
        std::string CommandId;
        ProjectCatalogOperation Operation{ ProjectCatalogOperation::RegisterLegacy };
        std::string ProjectId;
        uint64_t ExpectedVersion{};
        std::string Name;
        std::string Description;
        std::string RootPath;
    };
    struct ProjectCatalogCommandResult
    {
        std::string CommandId;
        std::string ProjectId;
        uint64_t Version{};
        uint64_t EventSequence{};
        bool Existing{};
        bool Replayed{};
    };
    [[nodiscard]] ProtocolRpcJsonDecodeResult<ProjectCatalogQueryRequest> DecodeProjectCatalogQueryRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<ProjectCatalogQueryResult> DecodeProjectCatalogQueryResult(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<ProjectCatalogCommandRequest> DecodeProjectCatalogCommandRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<ProjectCatalogCommandResult> DecodeProjectCatalogCommandResult(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeProjectCatalogQueryRequest(const ProjectCatalogQueryRequest& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeProjectCatalogQueryResult(const ProjectCatalogQueryResult& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeProjectCatalogCommandRequest(const ProjectCatalogCommandRequest& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeProjectCatalogCommandResult(const ProjectCatalogCommandResult& value);
}
