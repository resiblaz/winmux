// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime::details
{
    struct RuntimeLaunchLockSecurityResult
    {
        bool Succeeded{};
        DWORD SystemError{};
        std::string Error;
    };

    [[nodiscard]] RuntimeLaunchLockSecurityResult ValidateRuntimeLaunchLockSecurity(
        HANDLE mutex,
        std::wstring_view currentUserSid);
}
