// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolHandshake.h"

#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    enum class ProtocolJsonStatus
    {
        Succeeded,
        Oversized,
        InvalidUtf8,
        MalformedJson,
        InvalidSchema,
    };

    template<typename TValue>
    struct ProtocolJsonDecodeResult
    {
        ProtocolJsonStatus Status{ ProtocolJsonStatus::MalformedJson };
        TValue Value;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == ProtocolJsonStatus::Succeeded;
        }
    };

    struct ProtocolJsonEncodeResult
    {
        bool Succeeded{};
        std::string Payload;
        std::string Error;
    };

    [[nodiscard]] ProtocolJsonDecodeResult<HelloRequest> DecodeHelloRequest(std::string_view payload);
    [[nodiscard]] ProtocolJsonDecodeResult<HelloResponse> DecodeHelloResponse(std::string_view payload);
    [[nodiscard]] ProtocolJsonEncodeResult EncodeHelloRequest(const HelloRequest& request);
    [[nodiscard]] ProtocolJsonEncodeResult EncodeHelloResponse(const HelloResponse& response);
}
