// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"

#include <string>
#include <string_view>

namespace Microsoft::Terminal::Rtask
{
    inline constexpr std::wstring_view RtaskEndpointEnvironment{
        L"RICHER_TASK_ENDPOINT"
    };
    inline constexpr std::wstring_view RtaskRunIdEnvironment{
        L"RICHER_TASK_RUN_ID"
    };
    inline constexpr std::wstring_view RtaskCapabilityEnvironment{
        L"RICHER_TASK_CAPABILITY"
    };

    enum class RtaskContextStatus
    {
        Succeeded,
        MissingEnvironment,
        OversizedEnvironment,
        InvalidEndpoint,
        InvalidRun,
        InvalidCapability,
        SystemError,
    };

    struct RtaskContext
    {
        Microsoft::Terminal::TaskRuntime::RuntimeIdentity Identity;
        std::string RunId;
        std::string Capability;
    };

    struct RtaskContextResult
    {
        RtaskContextStatus Status{ RtaskContextStatus::SystemError };
        RtaskContext Context;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RtaskContextStatus::Succeeded;
        }
    };

    [[nodiscard]] RtaskContextResult ParseRtaskContext(
        std::wstring_view endpoint,
        std::string_view runId,
        std::string_view capability);
    [[nodiscard]] RtaskContextResult LoadRtaskContextFromEnvironment();
}
