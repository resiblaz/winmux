// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>

namespace Microsoft::Terminal::TaskRuntime
{
    // Shared process contract between TaskRuntime.exe and its activation
    // client. Values are stable because activation observes them out of
    // process before the control endpoint is ready.
    enum class RuntimeProcessExitCode : uint32_t
    {
        Success = 0,
        BootstrapFailure = 1,
        InvalidCommandLine = 2,
        AlreadyRunning = 3,
        RuntimeInstanceIdFailure = 4,
        ServerStartFailure = 5,
        UnexpectedServerStop = 6,
        StopInfrastructureFailure = 7,
    };

    [[nodiscard]] constexpr bool IsDeterministicRuntimeStartupFailureExitCode(
        const uint32_t exitCode) noexcept
    {
        switch (static_cast<RuntimeProcessExitCode>(exitCode))
        {
        case RuntimeProcessExitCode::BootstrapFailure:
        case RuntimeProcessExitCode::InvalidCommandLine:
        case RuntimeProcessExitCode::RuntimeInstanceIdFailure:
        case RuntimeProcessExitCode::ServerStartFailure:
        case RuntimeProcessExitCode::StopInfrastructureFailure:
            return true;
        default:
            return false;
        }
    }
}
