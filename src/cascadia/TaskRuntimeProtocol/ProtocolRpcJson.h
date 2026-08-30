// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolRpc.h"

#include <string>
#include <string_view>
#include <variant>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    enum class ProtocolRpcJsonStatus
    {
        Succeeded,
        UnknownNotification,
        Oversized,
        InvalidUtf8,
        MalformedJson,
        InvalidSchema,
    };

    template<typename TValue>
    struct ProtocolRpcJsonDecodeResult
    {
        ProtocolRpcJsonStatus Status{ ProtocolRpcJsonStatus::MalformedJson };
        TValue Value;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == ProtocolRpcJsonStatus::Succeeded;
        }
    };

    struct ProtocolRpcJsonEncodeResult
    {
        bool Succeeded{};
        std::string Payload;
        std::string Error;
    };

    using ClientControlEnvelope = std::variant<ControlResponse, ControlNotification>;

    [[nodiscard]] ProtocolRpcJsonDecodeResult<ControlRequest> DecodeControlRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<ControlResponse> DecodeControlResponse(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<ControlNotification> DecodeControlNotification(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<ClientControlEnvelope> DecodeClientControlEnvelope(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeControlRequest(const ControlRequest& request);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeControlResponse(const ControlResponse& response);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeControlNotification(const ControlNotification& notification);
}
