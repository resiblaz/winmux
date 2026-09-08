// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskBoardRpc.h"
#include "WorkspaceRpcJson.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    using namespace workspace_json;
    namespace
    {
        bool ReadTaskBoardCommandRequest(const Json::Value& root, TaskBoardCommandRequest& v)
        {
            if (!Exact(root, { "commandId", "taskId", "projectId", "expectedTaskVersion", "operation", "target", "expectedColumnRevision", "anchorId", "placement", "relatedRunId", "text", "acceptance", "undoOfCommandId" })) return false;
            if (!Id(root["commandId"], v.CommandId)) return false;
            if (!Id(root["taskId"], v.TaskId)) return false;
            if (!Id(root["projectId"], v.ProjectId)) return false;
            if (!UInt(root["expectedTaskVersion"], v.ExpectedTaskVersion)) return false;
            if (!Enum(root["operation"], v.Operation, 9)) return false;
            if (!Enum(root["target"], v.Target, 6)) return false;
            if (!UInt(root["expectedColumnRevision"], v.ExpectedColumnRevision)) return false;
            if (!Id(root["anchorId"], v.AnchorId)) return false;
            if (!Enum(root["placement"], v.Placement, 2)) return false;
            if (!Id(root["relatedRunId"], v.RelatedRunId)) return false;
            if (!Id(root["undoOfCommandId"], v.UndoOfCommandId)) return false;
            if (!Text(root["text"], v.Text, 65536)) return false;
            if (!root["acceptance"].isArray() || root["acceptance"].size() > MaximumTaskProjectionAcceptance) return false;
            std::array<bool, MaximumTaskProjectionAcceptance> seen{};
            for (const auto& item : root["acceptance"])
            {
                uint64_t index{}; bool satisfied{};
                if (!Exact(item, { "index", "satisfied" }) || !UInt(item["index"], index, MaximumTaskProjectionAcceptance - 1) ||
                    !Bool(item["satisfied"], satisfied) || seen[index]) return false;
                seen[index] = true;
                v.Acceptance.emplace_back(static_cast<uint32_t>(index), satisfied);
            }
            return v.ExpectedTaskVersion && v.ExpectedTaskVersion < MaxVersion &&
                       (v.Placement == TaskBoardPlacement::End ? !v.AnchorId : v.AnchorId.has_value()) &&
                       (!v.AnchorId || *v.AnchorId != v.TaskId) &&
                       ((v.Operation == TaskBoardOperation::Undo) == v.UndoOfCommandId.has_value()) &&
                       (!v.UndoOfCommandId || (*v.UndoOfCommandId != v.CommandId && !v.AnchorId && !v.RelatedRunId && v.Text.empty())) &&
                       (v.Acceptance.empty() || v.Operation == TaskBoardOperation::UpdateAcceptance);
        }
        Json::Value WriteTaskBoardCommandRequest(const TaskBoardCommandRequest& v)
        {
            Json::Value root{ Json::objectValue };
            root["commandId"] = v.CommandId;
            root["taskId"] = v.TaskId;
            root["projectId"] = v.ProjectId;
            root["expectedTaskVersion"] = Json::UInt64{ v.ExpectedTaskVersion };
            root["operation"] = static_cast<uint32_t>(v.Operation);
            root["target"] = static_cast<uint32_t>(v.Target);
            root["expectedColumnRevision"] = Json::UInt64{ v.ExpectedColumnRevision };
            root["anchorId"] = Optional(v.AnchorId);
            root["placement"] = static_cast<uint32_t>(v.Placement);
            root["relatedRunId"] = Optional(v.RelatedRunId);
            root["undoOfCommandId"] = Optional(v.UndoOfCommandId);
            root["text"] = v.Text;
            root["acceptance"] = Json::Value{ Json::arrayValue };
            for (const auto& [index, satisfied] : v.Acceptance)
            {
                Json::Value item{ Json::objectValue }; item["index"] = index; item["satisfied"] = satisfied;
                root["acceptance"].append(std::move(item));
            }
            return root;
        }

        bool ReadTaskBoardCommandResult(const Json::Value& root, TaskBoardCommandResult& v)
        {
            if (!Exact(root, { "commandId", "taskId", "taskVersion", "columnRevision", "eventSequence", "runId", "operationId", "pending", "replayed" })) return false;
            if (!Id(root["commandId"], v.CommandId)) return false;
            if (!Id(root["taskId"], v.TaskId)) return false;
            if (!UInt(root["taskVersion"], v.TaskVersion)) return false;
            if (!UInt(root["columnRevision"], v.ColumnRevision)) return false;
            if (!UInt(root["eventSequence"], v.EventSequence)) return false;
            if (!Id(root["runId"], v.RunId)) return false;
            if (!Id(root["operationId"], v.OperationId)) return false;
            if (!Bool(root["pending"], v.Pending)) return false;
            if (!Bool(root["replayed"], v.Replayed)) return false;
            return v.TaskVersion != 0;
        }
        Json::Value WriteTaskBoardCommandResult(const TaskBoardCommandResult& v)
        {
            Json::Value root{ Json::objectValue };
            root["commandId"] = v.CommandId;
            root["taskId"] = v.TaskId;
            root["taskVersion"] = Json::UInt64{ v.TaskVersion };
            root["columnRevision"] = Json::UInt64{ v.ColumnRevision };
            root["eventSequence"] = Json::UInt64{ v.EventSequence };
            root["runId"] = Optional(v.RunId);
            root["operationId"] = Optional(v.OperationId);
            root["pending"] = v.Pending;
            root["replayed"] = v.Replayed;
            return root;
        }

        bool ReadTaskBoardOperationQueryRequest(const Json::Value& root, TaskBoardOperationQueryRequest& v)
        {
            if (!Exact(root, { "operationId" })) return false;
            if (!Id(root["operationId"], v.OperationId)) return false;
            return true;
        }
        Json::Value WriteTaskBoardOperationQueryRequest(const TaskBoardOperationQueryRequest& v)
        {
            Json::Value root{ Json::objectValue };
            root["operationId"] = v.OperationId;
            return root;
        }

    }
    ProtocolRpcJsonDecodeResult<TaskBoardCommandRequest> DecodeTaskBoardCommandRequest(std::string_view payload)
    { return Decode<TaskBoardCommandRequest>(payload, ReadTaskBoardCommandRequest); }
    ProtocolRpcJsonEncodeResult EncodeTaskBoardCommandRequest(const TaskBoardCommandRequest& value)
    { return Encode(WriteTaskBoardCommandRequest(value), DecodeTaskBoardCommandRequest); }
    ProtocolRpcJsonDecodeResult<TaskBoardCommandResult> DecodeTaskBoardCommandResult(std::string_view payload)
    { return Decode<TaskBoardCommandResult>(payload, ReadTaskBoardCommandResult); }
    ProtocolRpcJsonEncodeResult EncodeTaskBoardCommandResult(const TaskBoardCommandResult& value)
    { return Encode(WriteTaskBoardCommandResult(value), DecodeTaskBoardCommandResult); }
    ProtocolRpcJsonDecodeResult<TaskBoardOperationQueryRequest> DecodeTaskBoardOperationQueryRequest(std::string_view payload)
    { return Decode<TaskBoardOperationQueryRequest>(payload, ReadTaskBoardOperationQueryRequest); }
    ProtocolRpcJsonEncodeResult EncodeTaskBoardOperationQueryRequest(const TaskBoardOperationQueryRequest& value)
    { return Encode(WriteTaskBoardOperationQueryRequest(value), DecodeTaskBoardOperationQueryRequest); }
}
