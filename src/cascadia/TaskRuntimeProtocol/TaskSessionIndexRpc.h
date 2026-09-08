// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "ProtocolRpcJson.h"
#include <vector>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    struct TaskSessionIndexQueryRequest { uint64_t AfterSequence{}; size_t Limit{ 100 }; };
    struct TaskSessionIndexProjection { uint64_t Sequence{}; std::string ProviderSessionId; };
    struct TaskSessionIndexQueryResult
    {
        std::vector<TaskSessionIndexProjection> Sessions;
        uint64_t AfterSequence{};
        bool HasMore{};
    };
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskSessionIndexQueryRequest> DecodeTaskSessionIndexQueryRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskSessionIndexQueryResult> DecodeTaskSessionIndexQueryResult(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskSessionIndexQueryRequest(const TaskSessionIndexQueryRequest& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskSessionIndexQueryResult(const TaskSessionIndexQueryResult& value);
}
