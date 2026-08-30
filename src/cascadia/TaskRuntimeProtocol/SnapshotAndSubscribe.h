// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolRpcJson.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    struct SnapshotAndSubscribeResult
    {
        std::string SubscriptionId;
        uint64_t Generation{};
        uint64_t Cursor{};
        std::string SnapshotJson{ "{}" };
    };

    struct UnsubscribeRequest
    {
        std::string SubscriptionId;
        uint64_t Generation{};
    };

    [[nodiscard]] ProtocolRpcJsonDecodeResult<SnapshotAndSubscribeResult>
        DecodeSnapshotAndSubscribeResult(std::string_view resultJson);
    [[nodiscard]] ProtocolRpcJsonEncodeResult
        EncodeSnapshotAndSubscribeResult(const SnapshotAndSubscribeResult& result);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<UnsubscribeRequest>
        DecodeUnsubscribeRequest(std::string_view paramsJson);
    [[nodiscard]] ProtocolRpcJsonEncodeResult
        EncodeUnsubscribeRequest(const UnsubscribeRequest& request);
}
