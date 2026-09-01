// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeMarker.h"

#include "GitWorktreeMetadataFile.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        GitWorktreeMarkerResult _failure(
            const GitWorktreeMarkerStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreeMarkerResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        GitWorktreeMarkerResult _fileFailure(
            const details::GitWorktreeMetadataFileResult& file)
        {
            using details::GitWorktreeMetadataFileStatus;
            GitWorktreeMarkerStatus status{ GitWorktreeMarkerStatus::IoFailure };
            switch (file.Status)
            {
            case GitWorktreeMetadataFileStatus::Missing:
                status = GitWorktreeMarkerStatus::Missing;
                break;
            case GitWorktreeMetadataFileStatus::Collision:
                status = GitWorktreeMarkerStatus::Collision;
                break;
            case GitWorktreeMetadataFileStatus::UnsafePath:
                status = GitWorktreeMarkerStatus::UnsafePath;
                break;
            case GitWorktreeMetadataFileStatus::ResourceFailure:
                status = GitWorktreeMarkerStatus::ResourceFailure;
                break;
            default:
                break;
            }
            return _failure(status, file.SystemError, file.Error);
        }

        bool _sameMarker(
            const GitWorktreeMarker& left,
            const GitWorktreeMarker& right) noexcept
        {
            return left.FormatVersion == right.FormatVersion &&
                   left.Task == right.Task &&
                   left.RepositoryIdentityDigest == right.RepositoryIdentityDigest &&
                   left.BranchRef == right.BranchRef &&
                   left.SnapshotDigest == right.SnapshotDigest &&
                   left.BaselineCommit == right.BaselineCommit &&
                   left.OwnershipNonce == right.OwnershipNonce &&
                   left.WorktreePath == right.WorktreePath;
        }

        bool _sameImmutableMarker(
            const GitWorktreeMarker& left,
            const GitWorktreeMarker& right) noexcept
        {
            return left.FormatVersion == right.FormatVersion &&
                   left.Task == right.Task &&
                   left.RepositoryIdentityDigest == right.RepositoryIdentityDigest &&
                   left.BranchRef == right.BranchRef &&
                   left.OwnershipNonce == right.OwnershipNonce &&
                   left.WorktreePath == right.WorktreePath &&
                   (left.SnapshotDigest.empty() ||
                    left.SnapshotDigest == right.SnapshotDigest) &&
                   (left.BaselineCommit.empty() ||
                    left.BaselineCommit == right.BaselineCommit);
        }

        bool _validateStateRoot(const GitWorktreeLayout& layout)
        {
            const auto current = ResolveCanonicalDirectoryIdentity(
                layout.StateRoot.CanonicalPath(),
                DirectoryIdentityPolicy::Worktree);
            return current.Succeeded() && layout.StateRoot.Valid() &&
                   current.Identity->Digest() == layout.StateRoot.Digest() &&
                   layout.MarkerPath.parent_path() ==
                       layout.StateRoot.CanonicalPath();
        }

        std::filesystem::path _temporaryMarkerPath(
            const GitWorktreeLayout& layout,
            const uint32_t attempt)
        {
            const auto name = std::wstring{
                                  layout.Task.Value().begin(),
                                  layout.Task.Value().end() } +
                              L".owner-v1." +
                              std::to_wstring(GetCurrentProcessId()) + L"." +
                              std::to_wstring(GetTickCount64()) + L"." +
                              std::to_wstring(attempt) + L".tmp";
            return layout.StateRoot.CanonicalPath() / name;
        }
    }

    GitWorktreeMarkerResult ReadGitWorktreeMarker(
        const GitWorktreeLayout& layout) noexcept
    {
        try
        {
            if (!_validateStateRoot(layout))
            {
                return _failure(
                    GitWorktreeMarkerStatus::UnsafePath,
                    ERROR_ACCESS_DENIED,
                    "Git Worktree marker state root identity changed");
            }
            const auto file = details::ReadGitWorktreeMetadataFile(
                layout.MarkerPath,
                MaximumGitWorktreeMarkerBytes);
            if (!file.Succeeded())
            {
                return _fileFailure(file);
            }
            const auto decoded = details::DecodeGitWorktreeMarker(file.Bytes);
            if (!decoded.Succeeded())
            {
                return decoded;
            }
            if (!ValidateGitWorktreeMarker(layout, *decoded.Marker))
            {
                return _failure(
                    GitWorktreeMarkerStatus::IdentityMismatch,
                    ERROR_INVALID_DATA,
                    "Git Worktree marker does not match its managed layout");
            }
            return decoded;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeMarkerStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree marker reading exhausted local resources");
        }
    }

    GitWorktreeMarkerResult CreateGitWorktreeMarker(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker) noexcept
    {
        try
        {
            if (!_validateStateRoot(layout) ||
                !ValidateGitWorktreeMarker(layout, marker))
            {
                return _failure(
                    GitWorktreeMarkerStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree marker creation request is invalid");
            }
            const auto encoded = details::EncodeGitWorktreeMarker(marker);
            if (!encoded)
            {
                return _failure(
                    GitWorktreeMarkerStatus::InvalidRequest,
                    ERROR_INVALID_DATA,
                    "Git Worktree marker could not be encoded");
            }
            const auto written = details::WriteNewGitWorktreeMetadataFile(
                layout.MarkerPath,
                *encoded);
            if (!written.Succeeded())
            {
                return _fileFailure(written);
            }
            const auto verified = ReadGitWorktreeMarker(layout);
            if (!verified.Succeeded() || !_sameMarker(*verified.Marker, marker))
            {
                return verified.Succeeded() ?
                           _failure(
                               GitWorktreeMarkerStatus::IdentityMismatch,
                               ERROR_INVALID_DATA,
                               "Created Git Worktree marker verification failed") :
                           verified;
            }
            return verified;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeMarkerStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree marker creation exhausted local resources");
        }
    }

    GitWorktreeMarkerResult ReplaceGitWorktreeMarker(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& expected,
        const GitWorktreeMarker& replacement) noexcept
    {
        try
        {
            if (!ValidateGitWorktreeMarker(layout, expected) ||
                !ValidateGitWorktreeMarker(layout, replacement) ||
                !_sameImmutableMarker(expected, replacement))
            {
                return _failure(
                    GitWorktreeMarkerStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree marker replacement request is invalid");
            }
            const auto current = ReadGitWorktreeMarker(layout);
            if (!current.Succeeded())
            {
                return current;
            }
            if (!_sameMarker(*current.Marker, expected))
            {
                return _failure(
                    GitWorktreeMarkerStatus::IdentityMismatch,
                    ERROR_REVISION_MISMATCH,
                    "Git Worktree marker changed before replacement");
            }
            const auto encoded = details::EncodeGitWorktreeMarker(replacement);
            if (!encoded)
            {
                return _failure(
                    GitWorktreeMarkerStatus::InvalidRequest,
                    ERROR_INVALID_DATA,
                    "Replacement Git Worktree marker could not be encoded");
            }

            std::filesystem::path temporary;
            details::GitWorktreeMetadataFileResult written;
            for (uint32_t attempt = 0; attempt < 16; ++attempt)
            {
                temporary = _temporaryMarkerPath(layout, attempt);
                written = details::WriteNewGitWorktreeMetadataFile(
                    temporary,
                    *encoded);
                if (written.Succeeded())
                {
                    break;
                }
                if (written.Status !=
                    details::GitWorktreeMetadataFileStatus::Collision)
                {
                    return _fileFailure(written);
                }
            }
            if (!written.Succeeded())
            {
                return _fileFailure(written);
            }

            const auto stillCurrent = ReadGitWorktreeMarker(layout);
            if (!stillCurrent.Succeeded() ||
                !_sameMarker(*stillCurrent.Marker, expected))
            {
                details::DeleteGitWorktreeMetadataFile(temporary);
                return stillCurrent.Succeeded() ?
                           _failure(
                               GitWorktreeMarkerStatus::IdentityMismatch,
                               ERROR_REVISION_MISMATCH,
                               "Git Worktree marker changed during replacement") :
                           stillCurrent;
            }
            const auto moved = details::ReplaceGitWorktreeMetadataFile(
                temporary,
                layout.MarkerPath);
            if (!moved.Succeeded())
            {
                return _fileFailure(moved);
            }
            const auto verified = ReadGitWorktreeMarker(layout);
            if (!verified.Succeeded() ||
                !_sameMarker(*verified.Marker, replacement))
            {
                return verified.Succeeded() ?
                           _failure(
                               GitWorktreeMarkerStatus::IdentityMismatch,
                               ERROR_INVALID_DATA,
                               "Replaced Git Worktree marker verification failed") :
                           verified;
            }
            return verified;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeMarkerStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree marker replacement exhausted local resources");
        }
    }
}
