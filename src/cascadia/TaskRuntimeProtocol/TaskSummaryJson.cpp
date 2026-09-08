// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskBoardRpc.h"
#include "WorkspaceRpcJson.h"
#include <unordered_set>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    using namespace workspace_json;
    namespace
    {
        bool ReadTaskSummaryProjection(const Json::Value& root, TaskSummaryProjection& v)
        {
            if (!Exact(root, { "id", "projectId", "projectName", "title", "status", "priority", "version", "manualHold", "updatedAt", "rank", "columnRevision", "executionContextId", "agentId", "agentRevision", "agentName", "model", "mode", "runId", "runState", "runSequence", "lastEventSequence", "hasPendingMessages", "hasPendingIntent", "archivedProject" })) return false;
            if (!Id(root["id"], v.Id)) return false;
            if (!Id(root["projectId"], v.ProjectId)) return false;
            if (!Text(root["projectName"], v.ProjectName, 1024)) return false;
            if (!Text(root["title"], v.Title, 1024, true)) return false;
            if (!Enum(root["status"], v.Status, 6)) return false;
            if (!Enum(root["priority"], v.Priority, 2)) return false;
            if (!UInt(root["version"], v.Version)) return false;
            if (!Bool(root["manualHold"], v.ManualHold)) return false;
            if (!Int(root["updatedAt"], v.UpdatedAtUnixMilliseconds)) return false;
            if (!Int(root["rank"], v.Rank)) return false;
            if (!UInt(root["columnRevision"], v.ColumnRevision)) return false;
            if (!Id(root["executionContextId"], v.ExecutionContextId)) return false;
            if (!Id(root["agentId"], v.AgentId, true)) return false;
            if (!UInt(root["agentRevision"], v.AgentRevision)) return false;
            if (!Text(root["agentName"], v.AgentName, 1024)) return false;
            if (!Text(root["model"], v.Model, 256)) return false;
            if (!Enum(root["mode"], v.Mode, 1)) return false;
            if (!Id(root["runId"], v.RunId, true)) return false;
            if (!Enum(root["runState"], v.RunState, 8)) return false;
            if (!UInt(root["runSequence"], v.RunSequence)) return false;
            if (!UInt(root["lastEventSequence"], v.LastEventSequence)) return false;
            if (!Bool(root["hasPendingMessages"], v.HasPendingMessages)) return false;
            if (!Bool(root["hasPendingIntent"], v.HasPendingIntent)) return false;
            if (!Bool(root["archivedProject"], v.ArchivedProject)) return false;
            return v.Version && v.ColumnRevision && (v.RunId.empty() == !v.RunState.has_value()) && (v.AgentId.empty() == (v.AgentRevision == 0)) && v.UpdatedAtUnixMilliseconds >= 0;
        }
        Json::Value WriteTaskSummaryProjection(const TaskSummaryProjection& v)
        {
            Json::Value root{ Json::objectValue };
            root["id"] = v.Id;
            root["projectId"] = v.ProjectId;
            root["projectName"] = v.ProjectName;
            root["title"] = v.Title;
            root["status"] = static_cast<uint32_t>(v.Status);
            root["priority"] = static_cast<uint32_t>(v.Priority);
            root["version"] = Json::UInt64{ v.Version };
            root["manualHold"] = v.ManualHold;
            root["updatedAt"] = Json::Int64{ v.UpdatedAtUnixMilliseconds };
            root["rank"] = Json::Int64{ v.Rank };
            root["columnRevision"] = Json::UInt64{ v.ColumnRevision };
            root["executionContextId"] = v.ExecutionContextId;
            root["agentId"] = v.AgentId;
            root["agentRevision"] = Json::UInt64{ v.AgentRevision };
            root["agentName"] = v.AgentName;
            root["model"] = v.Model;
            root["mode"] = static_cast<uint32_t>(v.Mode);
            root["runId"] = v.RunId;
            root["runState"] = OptionalEnum(v.RunState);
            root["runSequence"] = Json::UInt64{ v.RunSequence };
            root["lastEventSequence"] = Json::UInt64{ v.LastEventSequence };
            root["hasPendingMessages"] = v.HasPendingMessages;
            root["hasPendingIntent"] = v.HasPendingIntent;
            root["archivedProject"] = v.ArchivedProject;
            return root;
        }

        bool ReadTaskSummaryQueryRequest(const Json::Value& root, TaskSummaryQueryRequest& v)
        {
            if (!Exact(root, { "projectId", "status", "bucket", "openOnly", "search", "sort", "afterId", "afterValue", "expectedColumnRevision", "limit" })) return false;
            if (!Id(root["projectId"], v.ProjectId)) return false;
            if (!Enum(root["status"], v.Status, 6)) return false;
            if (!Enum(root["bucket"], v.Bucket, 6)) return false;
            if (!Bool(root["openOnly"], v.OpenOnly)) return false;
            if (!Text(root["search"], v.Search, 1024)) return false;
            if (!Enum(root["sort"], v.Sort, 1)) return false;
            if (!Id(root["afterId"], v.AfterId, true)) return false;
            if (!Int(root["afterValue"], v.AfterValue)) return false;
            if (!UInt(root["expectedColumnRevision"], v.ExpectedColumnRevision)) return false;
            if (!UInt(root["limit"], v.Limit, 100)) return false;
            return v.Limit && (v.Sort != TaskSummaryOrder::BoardOrder || (v.ProjectId && v.Bucket && *v.Bucket != TaskProjectionStatus::Blocked)) && (!v.AfterId.empty() || v.AfterValue == 0);
        }
        Json::Value WriteTaskSummaryQueryRequest(const TaskSummaryQueryRequest& v)
        {
            Json::Value root{ Json::objectValue };
            root["projectId"] = Optional(v.ProjectId);
            root["status"] = OptionalEnum(v.Status);
            root["bucket"] = OptionalEnum(v.Bucket);
            root["openOnly"] = v.OpenOnly;
            root["search"] = v.Search;
            root["sort"] = static_cast<uint32_t>(v.Sort);
            root["afterId"] = v.AfterId;
            root["afterValue"] = Json::Int64{ v.AfterValue };
            root["expectedColumnRevision"] = Json::UInt64{ v.ExpectedColumnRevision };
            root["limit"] = Json::UInt64{ v.Limit };
            return root;
        }

        bool ReadTaskSummaryQueryResult(const Json::Value& root, TaskSummaryQueryResult& v)
        {
            if (!Exact(root, { "tasks", "counts", "total", "generation", "columnRevision", "nextAfterId", "nextAfterValue" })) return false;
            if (!root["tasks"].isArray() || root["tasks"].size() > 100) return false;
            std::unordered_set<std::string> ids;
            for (const auto& item : root["tasks"])
            {
                TaskSummaryProjection parsed;
                if (!ReadTaskSummaryProjection(item, parsed)) return false;
                if (!ids.insert(parsed.Id).second) return false;
                v.Tasks.push_back(std::move(parsed));
            }
            if (!Numbers(root["counts"], v.Counts)) return false;
            if (!UInt(root["total"], v.Total)) return false;
            if (!UInt(root["generation"], v.Generation)) return false;
            if (!UInt(root["columnRevision"], v.ColumnRevision)) return false;
            if (!Id(root["nextAfterId"], v.NextAfterId, true)) return false;
            if (!Int(root["nextAfterValue"], v.NextAfterValue)) return false;
            uint64_t total{};
            for (const auto count : v.Counts)
            {
                if (count > MaxVersion - total) return false;
                total += count;
            }
            return v.Tasks.size() <= v.Total && v.Total == total &&
                   (v.NextAfterId.empty() || (!v.Tasks.empty() && v.NextAfterId == v.Tasks.back().Id));
        }
        Json::Value WriteTaskSummaryQueryResult(const TaskSummaryQueryResult& v)
        {
            Json::Value root{ Json::objectValue };
            root["tasks"] = Json::Value{ Json::arrayValue };
            for (const auto& item : v.Tasks) root["tasks"].append(WriteTaskSummaryProjection(item));
            root["counts"] = Numbers(v.Counts);
            root["total"] = Json::UInt64{ v.Total };
            root["generation"] = Json::UInt64{ v.Generation };
            root["columnRevision"] = Json::UInt64{ v.ColumnRevision };
            root["nextAfterId"] = v.NextAfterId;
            root["nextAfterValue"] = Json::Int64{ v.NextAfterValue };
            return root;
        }

    }
    ProtocolRpcJsonDecodeResult<TaskSummaryQueryRequest> DecodeTaskSummaryQueryRequest(std::string_view payload)
    { return Decode<TaskSummaryQueryRequest>(payload, ReadTaskSummaryQueryRequest); }
    ProtocolRpcJsonEncodeResult EncodeTaskSummaryQueryRequest(const TaskSummaryQueryRequest& value)
    { return Encode(WriteTaskSummaryQueryRequest(value), DecodeTaskSummaryQueryRequest); }
    ProtocolRpcJsonDecodeResult<TaskSummaryQueryResult> DecodeTaskSummaryQueryResult(std::string_view payload)
    { return Decode<TaskSummaryQueryResult>(payload, ReadTaskSummaryQueryResult); }
    ProtocolRpcJsonEncodeResult EncodeTaskSummaryQueryResult(const TaskSummaryQueryResult& value)
    { return Encode(WriteTaskSummaryQueryResult(value), DecodeTaskSummaryQueryResult); }
}
