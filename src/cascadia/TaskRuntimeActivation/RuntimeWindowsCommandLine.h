// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr size_t MaximumRuntimeWindowsCommandLineCharacters{ 32767 };

    enum class RuntimeWindowsCommandLineStatus
    {
        Succeeded,
        InvalidArgument,
        TooLong,
        ResourceFailure,
    };

    struct RuntimeWindowsCommandLineResult
    {
        RuntimeWindowsCommandLineStatus Status{ RuntimeWindowsCommandLineStatus::InvalidArgument };
        std::vector<wchar_t> MutableCommandLine;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeWindowsCommandLineStatus::Succeeded &&
                   !MutableCommandLine.empty() && MutableCommandLine.back() == L'\0';
        }
    };

    [[nodiscard]] RuntimeWindowsCommandLineResult BuildRuntimeWindowsCommandLine(
        std::span<const std::wstring> arguments) noexcept;
}
