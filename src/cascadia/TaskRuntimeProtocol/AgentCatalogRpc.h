// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "ProtocolRpcJson.h"
#include <vector>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr size_t MaximumAgentCatalogPageSize{ 100 };
    inline constexpr size_t MaximumAgentInstructionsBytes{ 32 * 1024 };

    struct AgentProfileReference
    {
        std::string Id;
        uint64_t Revision{};
    };

    struct AgentProfileConfiguration
    {
        std::string Name;
        std::string Description;
        std::string Provider{ "codex" };
        std::string Model;
        std::string Instructions;
    };

    struct AgentProfileProjection
    {
        std::string Id;
        uint64_t Version{};
        uint64_t Revision{};
        AgentProfileConfiguration Configuration;
        bool Archived{};
        int64_t CreatedAtUnixMilliseconds{};
        int64_t UpdatedAtUnixMilliseconds{};
        uint32_t ActiveRuns{};
        std::vector<std::string> DefaultWorkspaces;
    };

    struct WorkspaceAgentDefaultProjection
    {
        std::string WorkspaceId;
        std::optional<std::string> AgentId;
        uint64_t Version{};
    };

    struct AgentCatalogQueryRequest
    {
        std::optional<std::string> AgentId;
        std::optional<std::string> WorkspaceId;
        std::string AfterId;
        bool IncludeArchived{};
        size_t Limit{ 50 };
    };

    struct AgentCatalogQueryResult
    {
        std::vector<AgentProfileProjection> Profiles;
        std::optional<WorkspaceAgentDefaultProjection> Default;
        std::string NextAfterId;
        uint64_t Generation{};
    };

    enum class AgentCatalogOperation : uint8_t { Create, Update, Archive, Restore, SetWorkspaceDefault };

    struct AgentCatalogCommandRequest
    {
        std::string CommandId;
        AgentCatalogOperation Operation{ AgentCatalogOperation::Create };
        std::string AgentId;
        std::optional<std::string> WorkspaceId;
        uint64_t ExpectedVersion{};
        std::optional<AgentProfileConfiguration> Configuration;
    };

    struct AgentCatalogCommandResult
    {
        std::string CommandId;
        std::string AgentId;
        uint64_t Version{};
        uint64_t Revision{};
        uint64_t EventSequence{};
        bool Replayed{};
        uint32_t AffectedDefaults{};
    };

    struct TaskAgentConfiguration
    {
        std::optional<AgentProfileReference> Agent;
        std::string Provider{ "codex" };
        std::string Model;
        std::string Instructions;
    };

    [[nodiscard]] bool ValidateAgentCatalogQuery(const AgentCatalogQueryRequest& value, std::string& error);
    [[nodiscard]] bool ValidateAgentCatalogCommand(const AgentCatalogCommandRequest& value, std::string& error);
    [[nodiscard]] bool ValidateTaskAgentConfiguration(const TaskAgentConfiguration& value, std::string& error);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<AgentCatalogQueryRequest> DecodeAgentCatalogQueryRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<AgentCatalogQueryResult> DecodeAgentCatalogQueryResult(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<AgentCatalogCommandRequest> DecodeAgentCatalogCommandRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<AgentCatalogCommandResult> DecodeAgentCatalogCommandResult(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeAgentCatalogQueryRequest(const AgentCatalogQueryRequest& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeAgentCatalogQueryResult(const AgentCatalogQueryResult& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeAgentCatalogCommandRequest(const AgentCatalogCommandRequest& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeAgentCatalogCommandResult(const AgentCatalogCommandResult& value);
}
