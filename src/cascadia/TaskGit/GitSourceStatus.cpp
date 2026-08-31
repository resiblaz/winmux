// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitSourceStatus.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        bool _strictUtf8(const std::string_view value) noexcept
        {
            if (value.empty() ||
                value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return false;
            }
            return MultiByteToWideChar(
                       CP_UTF8,
                       MB_ERR_INVALID_CHARS,
                       value.data(),
                       static_cast<int>(value.size()),
                       nullptr,
                       0) > 0;
        }

        bool _changed(const char value) noexcept
        {
            return value != '.' && value != ' ';
        }

        std::vector<std::string_view> _tokens(
            const std::string_view output)
        {
            std::vector<std::string_view> result;
            size_t offset{};
            while (offset < output.size())
            {
                const auto end = output.find('\0', offset);
                if (end == std::string_view::npos)
                {
                    return {};
                }
                result.emplace_back(output.substr(offset, end - offset));
                offset = end + 1;
            }
            return result;
        }
    }

    GitStatusParseResult ParseGitStatusPorcelainV2(
        const std::string_view output) noexcept
    {
        GitStatusParseResult result;
        try
        {
            if (output.empty())
            {
                result.Succeeded = true;
                return result;
            }
            if (output.back() != '\0')
            {
                result.Error = "Git status output is not NUL terminated";
                return result;
            }
            const auto tokens = _tokens(output);
            if (tokens.empty())
            {
                result.Error = "Git status output tokenization failed";
                return result;
            }
            for (size_t index = 0; index < tokens.size(); ++index)
            {
                const auto token = tokens[index];
                if (token.empty() || !_strictUtf8(token))
                {
                    result.Error = "Git status record is empty or invalid UTF-8";
                    return result;
                }
                switch (token.front())
                {
                case '1':
                case 'u':
                    if (token.size() < 4 || token[1] != ' ')
                    {
                        result.Error = "Git status tracked record is malformed";
                        return result;
                    }
                    result.Summary.HasStagedChanges |= _changed(token[2]);
                    result.Summary.HasUnstagedChanges |= _changed(token[3]);
                    break;
                case '2':
                    if (token.size() < 4 || token[1] != ' ' ||
                        index + 1 >= tokens.size() || tokens[index + 1].empty() ||
                        !_strictUtf8(tokens[index + 1]))
                    {
                        result.Error = "Git status rename record is malformed";
                        return result;
                    }
                    result.Summary.HasStagedChanges |= _changed(token[2]);
                    result.Summary.HasUnstagedChanges |= _changed(token[3]);
                    ++index;
                    break;
                case '?':
                    if (token.size() < 3 || token[1] != ' ')
                    {
                        result.Error = "Git status untracked record is malformed";
                        return result;
                    }
                    ++result.Summary.UntrackedFiles;
                    break;
                case '!':
                    if (token.size() < 3 || token[1] != ' ')
                    {
                        result.Error = "Git status ignored record is malformed";
                        return result;
                    }
                    ++result.Summary.IgnoredFiles;
                    break;
                default:
                    result.Error = "Git status record kind is unsupported";
                    return result;
                }
            }
            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            result.Error = "Git status parsing exhausted local resources";
            return result;
        }
    }

    GitPathListParseResult ParseGitNulPathList(
        const std::string_view output,
        const size_t maximumPaths) noexcept
    {
        GitPathListParseResult result;
        try
        {
            if (maximumPaths == 0)
            {
                result.Error = "Git path-list maximum is invalid";
                return result;
            }
            if (output.empty())
            {
                result.Succeeded = true;
                return result;
            }
            if (output.back() != '\0')
            {
                result.Error = "Git path list is not NUL terminated";
                return result;
            }
            const auto tokens = _tokens(output);
            if (tokens.empty())
            {
                result.Error = "Git path-list tokenization failed";
                return result;
            }
            if (tokens.size() > maximumPaths)
            {
                result.CountExceeded = true;
                result.Error = "Git path list exceeds its file-count limit";
                return result;
            }
            result.Paths.reserve(tokens.size());
            for (const auto token : tokens)
            {
                if (token.empty() || token.size() > MaximumGitSnapshotPathBytes ||
                    !_strictUtf8(token))
                {
                    result.Error = "Git path-list entry is invalid";
                    return result;
                }
                if (std::ranges::find(result.Paths, token) != result.Paths.end())
                {
                    result.Error = "Git path list contains duplicate entries";
                    return result;
                }
                result.Paths.emplace_back(token);
            }
            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            result.Error = "Git path-list parsing exhausted local resources";
            return result;
        }
    }
}
