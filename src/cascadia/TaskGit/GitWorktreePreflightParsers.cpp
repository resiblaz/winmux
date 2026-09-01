// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreePreflight.h"

#include "GitSourceCommandOutput.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        using Microsoft::Terminal::TaskExecution::GitObjectFormat;

        bool _safeMetadata(const std::string_view value) noexcept
        {
            return !value.empty() &&
                   std::ranges::none_of(value, [](const unsigned char character) {
                       return character < 0x20 || character == 0x7f;
                   });
        }

        bool _validRegistration(
            const GitWorktreeRegistration& registration,
            const bool hasHead,
            const bool hasBranch,
            const GitObjectFormat format) noexcept
        {
            if (registration.Path.empty())
            {
                return false;
            }
            if (registration.Bare)
            {
                return !hasHead && !hasBranch && !registration.Detached;
            }
            return hasHead &&
                   IsGitSourceObjectId(registration.Head, format) &&
                   hasBranch != registration.Detached &&
                   (!hasBranch ||
                    (registration.BranchRef.starts_with("refs/heads/") &&
                     _safeMetadata(registration.BranchRef)));
        }

        bool _filterDriverName(const std::string_view value) noexcept
        {
            return !value.empty() && value.size() <= 128 &&
                   std::ranges::all_of(value, [](const unsigned char character) {
                       return (character >= 'a' && character <= 'z') ||
                              (character >= 'A' && character <= 'Z') ||
                              (character >= '0' && character <= '9') ||
                              character == '-' || character == '_' ||
                              character == '.';
                   });
        }

        std::optional<std::vector<std::string_view>> _lines(
            const std::string_view output) noexcept
        {
            try
            {
                std::vector<std::string_view> lines;
                size_t offset{};
                while (offset < output.size())
                {
                    const auto end = output.find('\n', offset);
                    auto line = output.substr(
                        offset,
                        end == std::string_view::npos ?
                            output.size() - offset :
                            end - offset);
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.remove_suffix(1);
                    }
                    if (line.empty())
                    {
                        return std::nullopt;
                    }
                    lines.emplace_back(line);
                    if (end == std::string_view::npos)
                    {
                        break;
                    }
                    offset = end + 1;
                }
                return lines;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
    }

    GitWorktreeListParseResult ParseGitWorktreeList(
        const std::string_view output,
        const GitObjectFormat objectFormat) noexcept
    {
        GitWorktreeListParseResult result;
        try
        {
            if (output.empty() || output.back() != '\0')
            {
                result.Error = "Git Worktree list is empty or not NUL terminated";
                return result;
            }

            GitWorktreeRegistration current;
            bool hasCurrent{};
            bool hasHead{};
            bool hasBranch{};
            bool hasDetached{};
            bool hasBare{};
            bool hasLocked{};
            bool hasPrunable{};
            const auto finish = [&]() -> bool {
                if (!hasCurrent ||
                    !_validRegistration(
                        current,
                        hasHead,
                        hasBranch,
                        objectFormat))
                {
                    return false;
                }
                result.Registrations.emplace_back(std::move(current));
                current = {};
                hasCurrent = false;
                hasHead = false;
                hasBranch = false;
                hasDetached = false;
                hasBare = false;
                hasLocked = false;
                hasPrunable = false;
                return true;
            };

            size_t offset{};
            while (offset < output.size())
            {
                const auto end = output.find('\0', offset);
                if (end == std::string_view::npos)
                {
                    result.Error = "Git Worktree list tokenization failed";
                    return result;
                }
                const auto token = output.substr(offset, end - offset);
                offset = end + 1;
                if (token.empty())
                {
                    if (!finish())
                    {
                        result.Error = "Git Worktree list record is incomplete";
                        return result;
                    }
                    continue;
                }

                if (!hasCurrent)
                {
                    constexpr std::string_view prefix{ "worktree " };
                    if (!token.starts_with(prefix) || token.size() == prefix.size())
                    {
                        result.Error = "Git Worktree list record has no path";
                        return result;
                    }
                    current.Path = std::string{ token.substr(prefix.size()) };
                    hasCurrent = true;
                    continue;
                }

                if (token.starts_with("HEAD "))
                {
                    if (hasHead || token.size() == 5)
                    {
                        result.Error = "Git Worktree list HEAD is duplicated or empty";
                        return result;
                    }
                    current.Head = std::string{ token.substr(5) };
                    hasHead = true;
                }
                else if (token.starts_with("branch "))
                {
                    if (hasBranch || hasDetached || token.size() == 7)
                    {
                        result.Error = "Git Worktree list branch state is invalid";
                        return result;
                    }
                    current.BranchRef = std::string{ token.substr(7) };
                    hasBranch = true;
                }
                else if (token == "detached")
                {
                    if (hasDetached || hasBranch)
                    {
                        result.Error = "Git Worktree list detached state is duplicated";
                        return result;
                    }
                    current.Detached = true;
                    hasDetached = true;
                }
                else if (token == "bare")
                {
                    if (hasBare)
                    {
                        result.Error = "Git Worktree list bare state is duplicated";
                        return result;
                    }
                    current.Bare = true;
                    hasBare = true;
                }
                else if (token == "locked" ||
                         (token.starts_with("locked ") && token.size() > 7))
                {
                    if (hasLocked)
                    {
                        result.Error = "Git Worktree list lock state is duplicated";
                        return result;
                    }
                    current.Locked = true;
                    hasLocked = true;
                }
                else if (token == "prunable" ||
                         (token.starts_with("prunable ") && token.size() > 9))
                {
                    if (hasPrunable)
                    {
                        result.Error = "Git Worktree list prune state is duplicated";
                        return result;
                    }
                    current.Prunable = true;
                    hasPrunable = true;
                }
                else
                {
                    result.Error = "Git Worktree list contains an unknown field";
                    return result;
                }
            }

            if (hasCurrent || result.Registrations.empty())
            {
                result.Error = "Git Worktree list has an unterminated record";
                return result;
            }
            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            result.Error = "Git Worktree list parsing exhausted local resources";
            return result;
        }
    }

    std::optional<bool> GitIndexContainsSubmodules(
        const std::string_view output,
        const GitObjectFormat objectFormat) noexcept
    {
        try
        {
            if (output.empty())
            {
                return false;
            }
            if (output.back() != '\0')
            {
                return std::nullopt;
            }

            bool found{};
            size_t offset{};
            while (offset < output.size())
            {
                const auto end = output.find('\0', offset);
                if (end == std::string_view::npos || end == offset)
                {
                    return std::nullopt;
                }
                const auto record = output.substr(offset, end - offset);
                offset = end + 1;
                const auto firstSpace = record.find(' ');
                const auto secondSpace = firstSpace == std::string_view::npos ?
                                             firstSpace :
                                             record.find(' ', firstSpace + 1);
                const auto tab = secondSpace == std::string_view::npos ?
                                     secondSpace :
                                     record.find('\t', secondSpace + 1);
                if (firstSpace != 6 || secondSpace == std::string_view::npos ||
                    tab != secondSpace + 2 || tab + 1 >= record.size())
                {
                    return std::nullopt;
                }
                const auto mode = record.substr(0, firstSpace);
                const auto object = record.substr(
                    firstSpace + 1,
                    secondSpace - firstSpace - 1);
                const auto stage = record[secondSpace + 1];
                if (!std::ranges::all_of(mode, [](const char character) {
                        return character >= '0' && character <= '7';
                    }) ||
                    !IsGitSourceObjectId(object, objectFormat) ||
                    stage < '0' || stage > '3')
                {
                    return std::nullopt;
                }
                found |= mode == "160000";
            }
            return found;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<bool> GitTreeContainsSubmodules(
        const std::string_view output,
        const GitObjectFormat objectFormat) noexcept
    {
        try
        {
            if (output.empty())
            {
                return false;
            }
            if (output.back() != '\0')
            {
                return std::nullopt;
            }

            bool found{};
            size_t offset{};
            while (offset < output.size())
            {
                const auto end = output.find('\0', offset);
                if (end == std::string_view::npos || end == offset)
                {
                    return std::nullopt;
                }
                const auto record = output.substr(offset, end - offset);
                offset = end + 1;
                const auto firstSpace = record.find(' ');
                const auto secondSpace = firstSpace == std::string_view::npos ?
                                             firstSpace :
                                             record.find(' ', firstSpace + 1);
                const auto tab = secondSpace == std::string_view::npos ?
                                     secondSpace :
                                     record.find('\t', secondSpace + 1);
                if (firstSpace != 6 || secondSpace == std::string_view::npos ||
                    tab == std::string_view::npos || tab + 1 >= record.size())
                {
                    return std::nullopt;
                }
                const auto mode = record.substr(0, firstSpace);
                const auto type = record.substr(
                    firstSpace + 1,
                    secondSpace - firstSpace - 1);
                const auto object = record.substr(
                    secondSpace + 1,
                    tab - secondSpace - 1);
                if (!std::ranges::all_of(mode, [](const char character) {
                        return character >= '0' && character <= '7';
                    }) ||
                    (type != "blob" && type != "tree" && type != "commit") ||
                    !IsGitSourceObjectId(object, objectFormat) ||
                    (mode == "160000" && type != "commit"))
                {
                    return std::nullopt;
                }
                found |= mode == "160000";
            }
            return found;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<std::vector<std::string>> ParseGitFilterDriverNames(
        const std::string_view output) noexcept
    {
        try
        {
            std::vector<std::string> result;
            const auto lines = _lines(output);
            if (!lines || lines->empty())
            {
                return std::nullopt;
            }
            for (const auto line : *lines)
            {
                const auto separator = line.find_first_of(" \t");
                if (separator == std::string_view::npos)
                {
                    return std::nullopt;
                }
                const auto key = line.substr(0, separator);
                constexpr std::string_view prefix{ "filter." };
                if (!key.starts_with(prefix))
                {
                    return std::nullopt;
                }
                const auto suffix = key.find_last_of('.');
                if (suffix == std::string_view::npos || suffix <= prefix.size())
                {
                    return std::nullopt;
                }
                const auto field = key.substr(suffix + 1);
                if (field != "clean" && field != "smudge" &&
                    field != "process" && field != "required")
                {
                    return std::nullopt;
                }
                const auto driver = key.substr(
                    prefix.size(),
                    suffix - prefix.size());
                if (!_filterDriverName(driver))
                {
                    return std::nullopt;
                }
                if (std::ranges::find(result, driver) == result.end())
                {
                    result.emplace_back(driver);
                }
            }
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<bool> GitConfigContainsTrueValue(
        const std::string_view output) noexcept
    {
        try
        {
            const auto lines = _lines(output);
            if (!lines || lines->empty())
            {
                return std::nullopt;
            }
            bool found{};
            for (const auto line : *lines)
            {
                const auto separator = line.find_last_of(" \t");
                if (separator == std::string_view::npos || separator == 0 ||
                    separator + 1 >= line.size())
                {
                    return std::nullopt;
                }
                const auto value = line.substr(separator + 1);
                if (value != "true" && value != "false")
                {
                    return std::nullopt;
                }
                found |= value == "true";
            }
            return found;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
