// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitSourceCommandOutput.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        constexpr size_t MaximumSourceCommandStdoutBytes{
            Microsoft::Terminal::TaskExecution::MaximumGitOutputBytes - 64 * 1024
        };
        constexpr size_t MaximumSourceCommandStderrBytes{ 64 * 1024 };
    }

    std::optional<std::string> ParseGitSourceLine(
        const Microsoft::Terminal::TaskExecution::GitCommandResult& result,
        const bool allowEmpty)
    {
        using namespace Microsoft::Terminal::TaskExecution;
        if (result.Status != GitCommandStatus::Completed ||
            result.Stdout.size() > MaximumSourceCommandStdoutBytes ||
            result.Stderr.size() > MaximumSourceCommandStderrBytes)
        {
            return std::nullopt;
        }
        auto value = result.Stdout;
        if (!value.empty() && value.back() == '\n')
        {
            value.pop_back();
            if (!value.empty() && value.back() == '\r')
            {
                value.pop_back();
            }
        }
        if ((!allowEmpty && value.empty()) ||
            std::ranges::any_of(value, [](const unsigned char character) {
                return character < 0x20 || character == 0x7f;
            }))
        {
            return std::nullopt;
        }
        return value;
    }

    bool IsGitSourceObjectId(
        const std::string_view value,
        const Microsoft::Terminal::TaskExecution::GitObjectFormat format) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitObjectFormat;
        const auto length = format == GitObjectFormat::Sha1 ? 40 : 64;
        return value.size() == length &&
               std::ranges::all_of(value, [](const char character) {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f');
               });
    }

    std::wstring WidenGitSourceAscii(const std::string_view value)
    {
        std::wstring result;
        result.reserve(value.size());
        for (const auto character : value)
        {
            result.push_back(static_cast<wchar_t>(
                static_cast<unsigned char>(character)));
        }
        return result;
    }
}
