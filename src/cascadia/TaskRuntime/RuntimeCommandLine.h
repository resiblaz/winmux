// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeCommandMode
    {
        BootstrapCheck,
        Serve,
        Help,
    };

    enum class RuntimeProcessExitCode : int
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

    struct RuntimeCommandLine
    {
        bool Succeeded{};
        RuntimeCommandMode Mode{ RuntimeCommandMode::Help };
        std::wstring Channel;
        std::filesystem::path SettingsRoot;
        std::optional<uintptr_t> TestReadyHandle;
        std::optional<uintptr_t> TestShutdownHandle;
        std::wstring Error;
    };

    [[nodiscard]] RuntimeCommandLine ParseRuntimeCommandLine(int argc, wchar_t* const* argv) noexcept;
}
