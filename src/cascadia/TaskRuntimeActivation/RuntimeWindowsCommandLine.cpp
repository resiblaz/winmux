// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeWindowsCommandLine.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        bool _requiresQuotes(const std::wstring_view argument) noexcept
        {
            return argument.empty() ||
                   argument.find_first_of(L" \t\n\v\f\r\"") != std::wstring_view::npos;
        }

        std::wstring _quoteArgument(const std::wstring_view argument)
        {
            if (!_requiresQuotes(argument))
            {
                return std::wstring{ argument };
            }

            std::wstring quoted{ L'\"' };
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
                    quoted.append(backslashes * 2 + 1, L'\\');
                    quoted.push_back(character);
                    backslashes = 0;
                    continue;
                }
                quoted.append(backslashes, L'\\');
                backslashes = 0;
                quoted.push_back(character);
            }
            quoted.append(backslashes * 2, L'\\');
            quoted.push_back(L'\"');
            return quoted;
        }

        RuntimeWindowsCommandLineResult _failure(
            const RuntimeWindowsCommandLineStatus status,
            std::string error)
        {
            RuntimeWindowsCommandLineResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }
    }

    RuntimeWindowsCommandLineResult BuildRuntimeWindowsCommandLine(
        const std::span<const std::wstring> arguments) noexcept
    {
        try
        {
            if (arguments.empty() || arguments.front().empty())
            {
                return _failure(
                    RuntimeWindowsCommandLineStatus::InvalidArgument,
                    "Windows argv must contain a non-empty executable argument");
            }

            std::wstring commandLine;
            for (const auto& argument : arguments)
            {
                if (argument.find(L'\0') != std::wstring::npos)
                {
                    return _failure(
                        RuntimeWindowsCommandLineStatus::InvalidArgument,
                        "Windows argv contains an embedded null");
                }
                if (argument.size() > MaximumRuntimeWindowsCommandLineCharacters - 1)
                {
                    return _failure(
                        RuntimeWindowsCommandLineStatus::TooLong,
                        "Windows command line exceeds the 32767-character limit");
                }
                auto quoted = _quoteArgument(argument);
                const auto separator = commandLine.empty() ? size_t{ 0 } : size_t{ 1 };
                const auto maximumPayload = MaximumRuntimeWindowsCommandLineCharacters - 1;
                const auto available = commandLine.size() <= maximumPayload ?
                                           maximumPayload - commandLine.size() :
                                           0;
                if (separator > available || quoted.size() > available - separator)
                {
                    return _failure(
                        RuntimeWindowsCommandLineStatus::TooLong,
                        "Windows command line exceeds the 32767-character limit");
                }
                if (separator != 0)
                {
                    commandLine.push_back(L' ');
                }
                commandLine.append(quoted);
            }

            RuntimeWindowsCommandLineResult result;
            result.MutableCommandLine.assign(commandLine.begin(), commandLine.end());
            result.MutableCommandLine.push_back(L'\0');
            if (result.MutableCommandLine.size() > MaximumRuntimeWindowsCommandLineCharacters)
            {
                return _failure(
                    RuntimeWindowsCommandLineStatus::TooLong,
                    "Windows command line exceeds the 32767-character limit");
            }
            result.Status = RuntimeWindowsCommandLineStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            return _failure(
                RuntimeWindowsCommandLineStatus::ResourceFailure,
                "Windows command-line construction failed");
        }
    }
}
