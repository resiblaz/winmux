// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RtaskCommand.h"

#include <span>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::Rtask
{
    struct RtaskCommandLine
    {
        bool Succeeded{};
        bool Help{};
        bool ReadsStdin{};
        Microsoft::Terminal::TaskRuntimeProtocol::RtaskMethod Method{
            Microsoft::Terminal::TaskRuntimeProtocol::RtaskMethod::StatusInProgress
        };
        std::wstring Error;
    };

    [[nodiscard]] RtaskCommandLine ParseRtaskCommandLine(
        std::span<const std::wstring_view> arguments);
    [[nodiscard]] RtaskCommandLine ParseRtaskCommandLine(
        int argc,
        wchar_t** argv);
}
