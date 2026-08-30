// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"
#include "RuntimePipe.h"
#include "RuntimeSecurity.h"
#include "RuntimeSingleInstance.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeBootstrapStatus
    {
        Ready,
        InvalidIdentity,
        InvalidSecurity,
        AlreadyRunning,
        SingleInstanceFailure,
        PipeFailure,
        UnexpectedFailure,
    };

    struct RuntimeBootstrapResult
    {
        RuntimeBootstrapStatus Status{ RuntimeBootstrapStatus::InvalidIdentity };
        RuntimeIdentity Identity;
        RuntimeSecurityAttributes Security;
        RuntimeSingleInstance Instance;
        RuntimeNamedPipe ControlPipe;
        std::string Error;

        [[nodiscard]] bool Ready() const noexcept
        {
            return Status == RuntimeBootstrapStatus::Ready;
        }
    };

    [[nodiscard]] RuntimeBootstrapResult BootstrapCurrentUserRuntime(
        std::wstring_view channel,
        const std::filesystem::path& settingsRoot);
}
