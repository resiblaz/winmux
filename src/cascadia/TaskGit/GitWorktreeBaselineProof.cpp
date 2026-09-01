// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeBaselineProof.h"

#include "GitSourceCommandOutput.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        bool _zeroObject(const std::string_view value, const size_t length) noexcept
        {
            return value.size() == length &&
                   std::ranges::all_of(value, [](const char character) {
                       return character == '0';
                   });
        }

        std::optional<std::array<std::string_view, 5>> _headerFields(
            const std::string_view header) noexcept
        {
            if (header.empty() || header.front() != ':')
            {
                return std::nullopt;
            }
            std::array<std::string_view, 5> fields;
            size_t offset{ 1 };
            for (size_t index = 0; index < fields.size(); ++index)
            {
                const auto end = index + 1 == fields.size() ?
                                     header.size() :
                                     header.find(' ', offset);
                if (end == std::string_view::npos || end == offset)
                {
                    return std::nullopt;
                }
                fields[index] = header.substr(offset, end - offset);
                offset = end + 1;
            }
            return fields;
        }
    }

    GitWorktreeTreeDiffParseResult ParseGitWorktreeTreeAdditions(
        const std::string_view output,
        const Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat) noexcept
    {
        GitWorktreeTreeDiffParseResult result;
        try
        {
            if (output.empty())
            {
                result.Succeeded = true;
                return result;
            }
            if (output.back() != '\0')
            {
                result.Error = "Git baseline tree diff is not NUL terminated";
                return result;
            }
            const auto objectLength =
                objectFormat ==
                        Microsoft::Terminal::TaskExecution::GitObjectFormat::Sha1 ?
                    size_t{ 40 } :
                    size_t{ 64 };
            size_t offset{};
            while (offset < output.size())
            {
                const auto headerEnd = output.find('\0', offset);
                if (headerEnd == std::string_view::npos || headerEnd == offset)
                {
                    result.Error = "Git baseline tree diff header is invalid";
                    return result;
                }
                const auto header = output.substr(offset, headerEnd - offset);
                offset = headerEnd + 1;
                const auto pathEnd = output.find('\0', offset);
                if (pathEnd == std::string_view::npos || pathEnd == offset)
                {
                    result.Error = "Git baseline tree diff path is invalid";
                    return result;
                }
                const auto path = output.substr(offset, pathEnd - offset);
                offset = pathEnd + 1;
                const auto fields = _headerFields(header);
                if (!fields || fields->at(0) != "000000" ||
                    (fields->at(1) != "100644" &&
                     fields->at(1) != "100755") ||
                    !_zeroObject(fields->at(2), objectLength) ||
                    !IsGitSourceObjectId(fields->at(3), objectFormat) ||
                    fields->at(4) != "A")
                {
                    result.Error =
                        "Git baseline tree diff contains a non-addition change";
                    return result;
                }
                result.Additions.push_back({
                    std::string{ path },
                    std::string{ fields->at(1) },
                    std::string{ fields->at(3) },
                });
            }
            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            result.Error = "Git baseline tree diff parsing exhausted local resources";
            return result;
        }
    }

    bool GitWorktreeBaselineCandidateMatches(
        const GitWorktreeReplayProof& proof,
        const GitWorktreeSourceJournal& journal,
        const std::span<const GitWorktreeTreeAddition> additions) noexcept
    {
        if (!proof.Status.HasStagedChanges ||
            proof.Status.HasUnstagedChanges ||
            proof.Status.UntrackedFiles != 0 ||
            proof.Status.IgnoredFiles != 0 ||
            proof.Tracked.WorktreeTree != proof.Tracked.IndexTree ||
            !GitWorktreeBaselineAdditionsMatch(journal, additions))
        {
            return false;
        }
        return true;
    }

    bool GitWorktreeBaselineAdditionsMatch(
        const GitWorktreeSourceJournal& journal,
        const std::span<const GitWorktreeTreeAddition> additions) noexcept
    {
        if (additions.size() != journal.UntrackedFiles.size())
        {
            return false;
        }
        for (size_t index = 0; index < additions.size(); ++index)
        {
            if (additions[index].Path !=
                journal.UntrackedFiles[index].RelativePath)
            {
                return false;
            }
        }
        return true;
    }

    std::string GitWorktreeBaselineSubject(const GitWorktreeMarker& marker)
    {
        return "Richer Task baseline " + marker.Task.Value() + " " +
               marker.OwnershipNonce;
    }

    bool ValidateGitWorktreeBaselineParents(
        const std::string_view output,
        const std::string_view expectedCommit,
        const std::string_view expectedParent,
        const Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat) noexcept
    {
        if (output.empty() || output.back() != '\n')
        {
            return false;
        }
        auto line = output.substr(0, output.size() - 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        const auto separator = line.find(' ');
        return separator != std::string_view::npos &&
               line.find(' ', separator + 1) == std::string_view::npos &&
               line.substr(0, separator) == expectedCommit &&
               line.substr(separator + 1) == expectedParent &&
               IsGitSourceObjectId(expectedCommit, objectFormat) &&
               IsGitSourceObjectId(expectedParent, objectFormat);
    }
}
