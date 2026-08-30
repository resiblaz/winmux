// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolRpcJson.h"
#include "RtaskCommand.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    [[nodiscard]] ProtocolRpcJsonDecodeResult<RtaskCommandRequest> DecodeRtaskCommandRequest(
        std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeRtaskCommandRequest(
        const RtaskCommandRequest& request);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<RtaskCommandResult> DecodeRtaskCommandResult(
        std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeRtaskCommandResult(
        const RtaskCommandResult& result);
}
