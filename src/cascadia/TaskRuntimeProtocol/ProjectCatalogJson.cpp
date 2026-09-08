// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "ProjectCatalogRpc.h"
#include "WorkspaceRpcJson.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    using namespace workspace_json;
    namespace
    {
        bool ReadProjectProjection(const Json::Value& root, ProjectProjection& v)
        {
            if (!Exact(root, { "id", "version", "name", "description", "kind", "binding", "rootPath", "archived", "legacy", "createdAt", "updatedAt", "defaultAgentId", "statusCounts", "columnRevisions", "activeRuns" })) return false;
            if (!Id(root["id"], v.Id)) return false;
            if (!UInt(root["version"], v.Version)) return false;
            if (!Text(root["name"], v.Name, 1024, true)) return false;
            if (!Text(root["description"], v.Description, 16384)) return false;
            if (!Enum(root["kind"], v.Kind, 1)) return false;
            if (!Enum(root["binding"], v.Binding, 1)) return false;
            if (!Text(root["rootPath"], v.RootPath, 32768)) return false;
            if (!Bool(root["archived"], v.Archived)) return false;
            if (!Bool(root["legacy"], v.Legacy)) return false;
            if (!Int(root["createdAt"], v.CreatedAtUnixMilliseconds)) return false;
            if (!Int(root["updatedAt"], v.UpdatedAtUnixMilliseconds)) return false;
            if (!Id(root["defaultAgentId"], v.DefaultAgentId)) return false;
            if (!Numbers(root["statusCounts"], v.StatusCounts)) return false;
            if (!Numbers(root["columnRevisions"], v.ColumnRevisions)) return false;
            if (!UInt(root["activeRuns"], v.ActiveRuns)) return false;
            return v.Version && v.CreatedAtUnixMilliseconds >= 0 && v.UpdatedAtUnixMilliseconds >= 0 && (v.Binding == 1 ? !v.RootPath.empty() : v.RootPath.empty());
        }
        Json::Value WriteProjectProjection(const ProjectProjection& v)
        {
            Json::Value root{ Json::objectValue };
            root["id"] = v.Id;
            root["version"] = Json::UInt64{ v.Version };
            root["name"] = v.Name;
            root["description"] = v.Description;
            root["kind"] = static_cast<uint32_t>(v.Kind);
            root["binding"] = static_cast<uint32_t>(v.Binding);
            root["rootPath"] = v.RootPath;
            root["archived"] = v.Archived;
            root["legacy"] = v.Legacy;
            root["createdAt"] = Json::Int64{ v.CreatedAtUnixMilliseconds };
            root["updatedAt"] = Json::Int64{ v.UpdatedAtUnixMilliseconds };
            root["defaultAgentId"] = Optional(v.DefaultAgentId);
            root["statusCounts"] = Numbers(v.StatusCounts);
            root["columnRevisions"] = Numbers(v.ColumnRevisions);
            root["activeRuns"] = Json::UInt64{ v.ActiveRuns };
            return root;
        }

        bool ReadProjectCatalogQueryRequest(const Json::Value& root, ProjectCatalogQueryRequest& v)
        {
            if (!Exact(root, { "projectId", "afterId", "includeArchived", "limit" })) return false;
            if (!Id(root["projectId"], v.ProjectId)) return false;
            if (!Id(root["afterId"], v.AfterId, true)) return false;
            if (!Bool(root["includeArchived"], v.IncludeArchived)) return false;
            if (!UInt(root["limit"], v.Limit, 100)) return false;
            return v.Limit && (!v.ProjectId || v.AfterId.empty());
        }
        Json::Value WriteProjectCatalogQueryRequest(const ProjectCatalogQueryRequest& v)
        {
            Json::Value root{ Json::objectValue };
            root["projectId"] = Optional(v.ProjectId);
            root["afterId"] = v.AfterId;
            root["includeArchived"] = v.IncludeArchived;
            root["limit"] = Json::UInt64{ v.Limit };
            return root;
        }

        bool ReadProjectCatalogQueryResult(const Json::Value& root, ProjectCatalogQueryResult& v)
        {
            if (!Exact(root, { "projects", "nextAfterId", "generation" })) return false;
            if (!root["projects"].isArray() || root["projects"].size() > 100) return false;
            for (const auto& item : root["projects"])
            {
                ProjectProjection parsed;
                if (!ReadProjectProjection(item, parsed)) return false;
                if (!v.Projects.empty() && v.Projects.back().Id >= parsed.Id) return false;
                v.Projects.push_back(std::move(parsed));
            }
            if (!Id(root["nextAfterId"], v.NextAfterId, true)) return false;
            if (!UInt(root["generation"], v.Generation)) return false;
            return true;
        }
        Json::Value WriteProjectCatalogQueryResult(const ProjectCatalogQueryResult& v)
        {
            Json::Value root{ Json::objectValue };
            root["projects"] = Json::Value{ Json::arrayValue };
            for (const auto& item : v.Projects) root["projects"].append(WriteProjectProjection(item));
            root["nextAfterId"] = v.NextAfterId;
            root["generation"] = Json::UInt64{ v.Generation };
            return root;
        }

        bool ReadProjectCatalogCommandRequest(const Json::Value& root, ProjectCatalogCommandRequest& v)
        {
            if (!Exact(root, { "commandId", "operation", "projectId", "expectedVersion", "name", "description", "rootPath" })) return false;
            if (!Id(root["commandId"], v.CommandId)) return false;
            if (!Enum(root["operation"], v.Operation, 5)) return false;
            if (!Id(root["projectId"], v.ProjectId)) return false;
            if (!UInt(root["expectedVersion"], v.ExpectedVersion)) return false;
            if (!Text(root["name"], v.Name, 1024)) return false;
            if (!Text(root["description"], v.Description, 16384)) return false;
            if (!Text(root["rootPath"], v.RootPath, 32768)) return false;
            return v.ExpectedVersion < MaxVersion &&
                       ((v.Operation == ProjectCatalogOperation::Create || v.Operation == ProjectCatalogOperation::RegisterLegacy) ? v.ExpectedVersion == 0 : v.ExpectedVersion != 0) &&
                       ((v.Operation == ProjectCatalogOperation::Create || v.Operation == ProjectCatalogOperation::UpdateMetadata || v.Operation == ProjectCatalogOperation::RegisterLegacy) ? !v.Name.empty() : true) &&
                       ((v.Operation == ProjectCatalogOperation::Create || v.Operation == ProjectCatalogOperation::BindDirectory) ? !v.RootPath.empty() : v.RootPath.empty());
        }
        Json::Value WriteProjectCatalogCommandRequest(const ProjectCatalogCommandRequest& v)
        {
            Json::Value root{ Json::objectValue };
            root["commandId"] = v.CommandId;
            root["operation"] = static_cast<uint32_t>(v.Operation);
            root["projectId"] = v.ProjectId;
            root["expectedVersion"] = Json::UInt64{ v.ExpectedVersion };
            root["name"] = v.Name;
            root["description"] = v.Description;
            root["rootPath"] = v.RootPath;
            return root;
        }

        bool ReadProjectCatalogCommandResult(const Json::Value& root, ProjectCatalogCommandResult& v)
        {
            if (!Exact(root, { "commandId", "projectId", "version", "eventSequence", "existing", "replayed" })) return false;
            if (!Id(root["commandId"], v.CommandId)) return false;
            if (!Id(root["projectId"], v.ProjectId)) return false;
            if (!UInt(root["version"], v.Version)) return false;
            if (!UInt(root["eventSequence"], v.EventSequence)) return false;
            if (!Bool(root["existing"], v.Existing)) return false;
            if (!Bool(root["replayed"], v.Replayed)) return false;
            return v.Version && v.EventSequence;
        }
        Json::Value WriteProjectCatalogCommandResult(const ProjectCatalogCommandResult& v)
        {
            Json::Value root{ Json::objectValue };
            root["commandId"] = v.CommandId;
            root["projectId"] = v.ProjectId;
            root["version"] = Json::UInt64{ v.Version };
            root["eventSequence"] = Json::UInt64{ v.EventSequence };
            root["existing"] = v.Existing;
            root["replayed"] = v.Replayed;
            return root;
        }

    }
    ProtocolRpcJsonDecodeResult<ProjectCatalogQueryRequest> DecodeProjectCatalogQueryRequest(std::string_view payload)
    { return Decode<ProjectCatalogQueryRequest>(payload, ReadProjectCatalogQueryRequest); }
    ProtocolRpcJsonEncodeResult EncodeProjectCatalogQueryRequest(const ProjectCatalogQueryRequest& value)
    { return Encode(WriteProjectCatalogQueryRequest(value), DecodeProjectCatalogQueryRequest); }
    ProtocolRpcJsonDecodeResult<ProjectCatalogQueryResult> DecodeProjectCatalogQueryResult(std::string_view payload)
    { return Decode<ProjectCatalogQueryResult>(payload, ReadProjectCatalogQueryResult); }
    ProtocolRpcJsonEncodeResult EncodeProjectCatalogQueryResult(const ProjectCatalogQueryResult& value)
    { return Encode(WriteProjectCatalogQueryResult(value), DecodeProjectCatalogQueryResult); }
    ProtocolRpcJsonDecodeResult<ProjectCatalogCommandRequest> DecodeProjectCatalogCommandRequest(std::string_view payload)
    { return Decode<ProjectCatalogCommandRequest>(payload, ReadProjectCatalogCommandRequest); }
    ProtocolRpcJsonEncodeResult EncodeProjectCatalogCommandRequest(const ProjectCatalogCommandRequest& value)
    { return Encode(WriteProjectCatalogCommandRequest(value), DecodeProjectCatalogCommandRequest); }
    ProtocolRpcJsonDecodeResult<ProjectCatalogCommandResult> DecodeProjectCatalogCommandResult(std::string_view payload)
    { return Decode<ProjectCatalogCommandResult>(payload, ReadProjectCatalogCommandResult); }
    ProtocolRpcJsonEncodeResult EncodeProjectCatalogCommandResult(const ProjectCatalogCommandResult& value)
    { return Encode(WriteProjectCatalogCommandResult(value), DecodeProjectCatalogCommandResult); }
}
