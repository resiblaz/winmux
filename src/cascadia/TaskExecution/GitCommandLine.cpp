// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitCommandLine.h"

#include "GitCommandTypes.h"

namespace Microsoft::Terminal::TaskExecution::details
{
    std::optional<std::wstring> QuoteWindowsArgument(
        const std::wstring_view argument) noexcept
    {
        try
        {
            if (argument.size() > MaximumGitArgumentCharacters ||
                argument.find(L'\0') != std::wstring_view::npos)
            {
                return std::nullopt;
            }

            std::wstring result;
            result.reserve(argument.size() + 2);
            result.push_back(L'\"');
            size_t backslashes{};
            for (const auto character : argument)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'\"')
                {
                    result.append(backslashes * 2 + 1, L'\\');
                    result.push_back(L'\"');
                    backslashes = 0;
                    continue;
                }
                result.append(backslashes, L'\\');
                backslashes = 0;
                result.push_back(character);
            }
            result.append(backslashes * 2, L'\\');
            result.push_back(L'\"');
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<std::wstring> BuildGitCommandLine(
        const std::filesystem::path& executable,
        const std::span<const std::wstring> arguments) noexcept
    {
        try
        {
            const auto executableArgument = QuoteWindowsArgument(executable.native());
            if (!executableArgument || arguments.empty() ||
                arguments.size() > MaximumGitArgumentCount)
            {
                return std::nullopt;
            }

            std::wstring result{ *executableArgument };
            for (const auto& argument : arguments)
            {
                const auto quoted = QuoteWindowsArgument(argument);
                if (!quoted || result.size() + 1 + quoted->size() >
                                   MaximumGitCommandLineCharacters)
                {
                    return std::nullopt;
                }
                result.push_back(L' ');
                result.append(*quoted);
            }
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
