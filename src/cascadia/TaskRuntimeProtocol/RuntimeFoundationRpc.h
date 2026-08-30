// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolRpcJson.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr std::string_view RuntimeFoundationReadyState{ "ready" };

    struct PingResult
    {
        std::string RuntimeInstanceId;
        std::string ClientInstanceId;
    };

    struct RuntimeStatusResult
    {
        std::string RuntimeIdentity;
        std::string RuntimeInstanceId;
        uint32_t ProcessId{};
        std::string State{ RuntimeFoundationReadyState };
    };

    [[nodiscard]] ProtocolRpcJsonDecodeResult<PingResult>
        DecodePingResult(std::string_view resultJson);
    [[nodiscard]] ProtocolRpcJsonEncodeResult
        EncodePingResult(const PingResult& result);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<RuntimeStatusResult>
        DecodeRuntimeStatusResult(std::string_view resultJson);
    [[nodiscard]] ProtocolRpcJsonEncodeResult
        EncodeRuntimeStatusResult(const RuntimeStatusResult& result);
}
