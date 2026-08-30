// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeTerminalEndpointStatus
    {
        Succeeded,
        InvalidRuntimeIdentity,
        InvalidTerminalSessionId,
        InvalidGeneration,
        ResourceFailure,
    };

    struct RuntimeTerminalEndpoint
    {
        std::wstring PipeName;
        std::string RuntimeIdentity;
        std::string TerminalSessionId;
        uint64_t Generation{};
    };

    struct RuntimeTerminalEndpointResult
    {
        RuntimeTerminalEndpointStatus Status{
            RuntimeTerminalEndpointStatus::InvalidRuntimeIdentity
        };
        RuntimeTerminalEndpoint Endpoint;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeTerminalEndpointStatus::Succeeded;
        }
    };

    [[nodiscard]] RuntimeTerminalEndpointResult BuildRuntimeTerminalEndpoint(
        const RuntimeIdentity& runtimeIdentity,
        std::string_view terminalSessionId,
        uint64_t generation);
    [[nodiscard]] bool ValidateRuntimeTerminalEndpoint(
        const RuntimeTerminalEndpoint& endpoint) noexcept;
}
