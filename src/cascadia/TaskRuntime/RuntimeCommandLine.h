// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeProcessExitCode.h"

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

    struct RuntimeCommandLine
    {
        bool Succeeded{};
        RuntimeCommandMode Mode{ RuntimeCommandMode::Help };
        std::wstring Channel;
        std::filesystem::path SettingsRoot;
        std::optional<uintptr_t> TestReadyHandle;
        std::optional<uintptr_t> TestShutdownHandle;
        std::optional<uint32_t> TestIdleExitMilliseconds;
        std::wstring Error;
    };

    [[nodiscard]] RuntimeCommandLine ParseRuntimeCommandLine(int argc, wchar_t* const* argv) noexcept;
}
