// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskSessionIndexRpc.h"
#include "WorkspaceRpcJson.h"
#include <unordered_set>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    using namespace workspace_json;
    ProtocolRpcJsonDecodeResult<TaskSessionIndexQueryRequest> DecodeTaskSessionIndexQueryRequest(std::string_view payload)
    {
        return Decode<TaskSessionIndexQueryRequest>(payload, [](const auto& root, auto& value) {
            return Exact(root, { "afterSequence", "limit" }) && UInt(root["afterSequence"], value.AfterSequence) &&
                   UInt(root["limit"], value.Limit, 100) && value.Limit;
        });
    }
    ProtocolRpcJsonEncodeResult EncodeTaskSessionIndexQueryRequest(const TaskSessionIndexQueryRequest& value)
    {
        Json::Value root{ Json::objectValue }; root["afterSequence"] = Json::UInt64{ value.AfterSequence }; root["limit"] = Json::UInt64{ value.Limit };
        return Encode(root, DecodeTaskSessionIndexQueryRequest);
    }
    ProtocolRpcJsonDecodeResult<TaskSessionIndexQueryResult> DecodeTaskSessionIndexQueryResult(std::string_view payload)
    {
        return Decode<TaskSessionIndexQueryResult>(payload, [](const auto& root, auto& value) {
            if (!Exact(root, { "sessions", "afterSequence", "hasMore" }) || !root["sessions"].isArray() || root["sessions"].size() > 100 ||
                !UInt(root["afterSequence"], value.AfterSequence) || !Bool(root["hasMore"], value.HasMore)) return false;
            uint64_t sequence{}; std::unordered_set<std::string> sessions;
            for (const auto& row : root["sessions"])
            {
                TaskSessionIndexProjection entry;
                if (!Exact(row, { "sequence", "providerSessionId" }) || !UInt(row["sequence"], entry.Sequence) ||
                    entry.Sequence <= sequence || !Text(row["providerSessionId"], entry.ProviderSessionId, 256, true) || !sessions.insert(entry.ProviderSessionId).second) return false;
                sequence = entry.Sequence; value.Sessions.push_back(std::move(entry));
            }
            return value.Sessions.empty() ? !value.HasMore : sequence == value.AfterSequence;
        });
    }
    ProtocolRpcJsonEncodeResult EncodeTaskSessionIndexQueryResult(const TaskSessionIndexQueryResult& value)
    {
        Json::Value root{ Json::objectValue }; root["sessions"] = Json::Value{ Json::arrayValue };
        for (const auto& entry : value.Sessions)
        {
            Json::Value row{ Json::objectValue }; row["sequence"] = Json::UInt64{ entry.Sequence }; row["providerSessionId"] = entry.ProviderSessionId;
            root["sessions"].append(std::move(row));
        }
        root["afterSequence"] = Json::UInt64{ value.AfterSequence }; root["hasMore"] = value.HasMore;
        return Encode(root, DecodeTaskSessionIndexQueryResult);
    }
}
