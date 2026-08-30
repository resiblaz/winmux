// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdio>
#include <string>

namespace Microsoft::Terminal::Rtask
{
    enum class RtaskStdinStatus
    {
        Succeeded,
        Empty,
        Oversized,
        ReadFailure,
        ResourceFailure,
    };

    struct RtaskStdinResult
    {
        RtaskStdinStatus Status{ RtaskStdinStatus::ReadFailure };
        std::string Text;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RtaskStdinStatus::Succeeded;
        }
    };

    [[nodiscard]] RtaskStdinResult ReadRtaskStdin(
        FILE* stream,
        size_t maximumBytes);
}
