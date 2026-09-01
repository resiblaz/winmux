// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeSourceJournal.h"

#include "GitSourceDigest.h"
#include "GitWorktreeMetadataFile.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        GitWorktreeSourceJournalResult _failure(
            const GitWorktreeSourceJournalStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreeSourceJournalResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        GitWorktreeSourceJournalResult _fileFailure(
            const details::GitWorktreeMetadataFileResult& file)
        {
            using details::GitWorktreeMetadataFileStatus;
            GitWorktreeSourceJournalStatus status{
                GitWorktreeSourceJournalStatus::IoFailure
            };
            switch (file.Status)
            {
            case GitWorktreeMetadataFileStatus::Missing:
                status = GitWorktreeSourceJournalStatus::Missing;
                break;
            case GitWorktreeMetadataFileStatus::Collision:
                status = GitWorktreeSourceJournalStatus::Collision;
                break;
            case GitWorktreeMetadataFileStatus::UnsafePath:
                status = GitWorktreeSourceJournalStatus::UnsafePath;
                break;
            case GitWorktreeMetadataFileStatus::ResourceFailure:
                status = GitWorktreeSourceJournalStatus::ResourceFailure;
                break;
            default:
                break;
            }
            return _failure(status, file.SystemError, file.Error);
        }

        std::optional<std::string> _utf8Path(
            const std::filesystem::path& path)
        {
            const auto& value = path.native();
            if (value.empty() ||
                value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return std::nullopt;
            }
            const auto required = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (required <= 0)
            {
                return std::nullopt;
            }
            std::string result(static_cast<size_t>(required), '\0');
            return WideCharToMultiByte(
                       CP_UTF8,
                       WC_ERR_INVALID_CHARS,
                       value.data(),
                       static_cast<int>(value.size()),
                       result.data(),
                       required,
                       nullptr,
                       nullptr) == required ?
                       std::optional<std::string>{ std::move(result) } :
                       std::nullopt;
        }

        bool _sameJournal(
            const GitWorktreeSourceJournal& left,
            const GitWorktreeSourceJournal& right) noexcept
        {
            return left.FormatVersion == right.FormatVersion &&
                   left.Task == right.Task &&
                   left.OwnershipNonce == right.OwnershipNonce &&
                   left.RepositoryIdentityDigest == right.RepositoryIdentityDigest &&
                   left.SourceWorktreePath == right.SourceWorktreePath &&
                   left.ObjectFormat == right.ObjectFormat &&
                   left.SourceHead == right.SourceHead &&
                   left.SourceReference == right.SourceReference &&
                   left.TrackedSnapshotCommit == right.TrackedSnapshotCommit &&
                   left.WorktreeTree == right.WorktreeTree &&
                   left.IndexTree == right.IndexTree &&
                   left.StashReference == right.StashReference &&
                   left.StatusDigest == right.StatusDigest &&
                   left.SnapshotDigest == right.SnapshotDigest &&
                   left.UntrackedBytes == right.UntrackedBytes &&
                   left.UntrackedFiles.size() == right.UntrackedFiles.size() &&
                   std::equal(
                       left.UntrackedFiles.begin(),
                       left.UntrackedFiles.end(),
                       right.UntrackedFiles.begin(),
                       [](const auto& first, const auto& second) {
                           return first.RelativePath == second.RelativePath &&
                                  first.Type == second.Type &&
                                  first.Size == second.Size &&
                                  first.Sha256 == second.Sha256;
                       });
        }

        bool _validateStateRoot(const GitWorktreeLayout& layout)
        {
            const auto current = ResolveCanonicalDirectoryIdentity(
                layout.StateRoot.CanonicalPath(),
                DirectoryIdentityPolicy::Worktree);
            return current.Succeeded() && layout.StateRoot.Valid() &&
                   current.Identity->Digest() == layout.StateRoot.Digest() &&
                   layout.SnapshotPath.parent_path() ==
                       layout.StateRoot.CanonicalPath();
        }
    }

    bool ValidateGitWorktreeSourceJournal(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal) noexcept
    {
        return ValidateGitWorktreeMarker(layout, marker) &&
               details::ValidateGitWorktreeSourceJournalShape(journal) &&
               journal.Task == layout.Task && journal.Task == marker.Task &&
               journal.OwnershipNonce == marker.OwnershipNonce &&
               journal.RepositoryIdentityDigest ==
                   marker.RepositoryIdentityDigest &&
               (marker.SnapshotDigest.empty() ||
                marker.SnapshotDigest == journal.SnapshotDigest);
    }

    GitWorktreeSourceJournalResult MakeGitWorktreeSourceJournal(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitSourceSnapshot& snapshot) noexcept
    {
        try
        {
            const auto sourcePath = _utf8Path(
                snapshot.Repository.WorktreeIdentity.CanonicalPath());
            const auto snapshotDigest =
                details::ComputeGitSourceSnapshotDigest(snapshot);
            if (!sourcePath || !ValidateGitWorktreeMarker(layout, marker) ||
                snapshot.Repository.CommonDirectoryIdentity.Digest() !=
                    marker.RepositoryIdentityDigest ||
                !snapshotDigest || *snapshotDigest != snapshot.SnapshotDigest)
            {
                return _failure(
                    GitWorktreeSourceJournalStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree source journal request is invalid");
            }

            GitWorktreeSourceJournal journal;
            journal.Task = layout.Task;
            journal.OwnershipNonce = marker.OwnershipNonce;
            journal.RepositoryIdentityDigest = marker.RepositoryIdentityDigest;
            journal.SourceWorktreePath = *sourcePath;
            journal.ObjectFormat = snapshot.Repository.ObjectFormat;
            journal.SourceHead = snapshot.Repository.Head;
            journal.SourceReference = snapshot.Repository.BranchRef;
            journal.TrackedSnapshotCommit = snapshot.TrackedSnapshotCommit;
            journal.WorktreeTree = snapshot.WorktreeTree;
            journal.IndexTree = snapshot.IndexTree;
            journal.StashReference = snapshot.StashReference;
            journal.StatusDigest = snapshot.StatusDigest;
            journal.SnapshotDigest = *snapshotDigest;
            journal.UntrackedFiles = snapshot.UntrackedFiles;
            journal.UntrackedBytes = snapshot.UntrackedBytes;
            if (!ValidateGitWorktreeSourceJournal(layout, marker, journal))
            {
                return _failure(
                    GitWorktreeSourceJournalStatus::InvalidRequest,
                    ERROR_INVALID_DATA,
                    "Git source snapshot cannot form a valid Worktree journal");
            }

            GitWorktreeSourceJournalResult result;
            result.Status = GitWorktreeSourceJournalStatus::Succeeded;
            result.Journal = std::move(journal);
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeSourceJournalStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree source journal allocation failed");
        }
    }

    GitWorktreeSourceJournalResult ReadGitWorktreeSourceJournal(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker) noexcept
    {
        try
        {
            if (!_validateStateRoot(layout) ||
                !ValidateGitWorktreeMarker(layout, marker))
            {
                return _failure(
                    GitWorktreeSourceJournalStatus::UnsafePath,
                    ERROR_ACCESS_DENIED,
                    "Git Worktree source journal state root is unsafe");
            }
            const auto file = details::ReadGitWorktreeMetadataFile(
                layout.SnapshotPath,
                MaximumGitWorktreeSourceJournalBytes);
            if (!file.Succeeded())
            {
                return _fileFailure(file);
            }
            const auto decoded = details::DecodeGitWorktreeSourceJournal(
                file.Bytes);
            if (!decoded.Succeeded())
            {
                return decoded;
            }
            if (!ValidateGitWorktreeSourceJournal(
                    layout,
                    marker,
                    *decoded.Journal))
            {
                return _failure(
                    GitWorktreeSourceJournalStatus::IdentityMismatch,
                    ERROR_INVALID_DATA,
                    "Git Worktree source journal identity does not match");
            }
            return decoded;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeSourceJournalStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree source journal reading exhausted local resources");
        }
    }

    GitWorktreeSourceJournalResult CreateGitWorktreeSourceJournal(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal) noexcept
    {
        try
        {
            if (!_validateStateRoot(layout) ||
                !ValidateGitWorktreeSourceJournal(layout, marker, journal))
            {
                return _failure(
                    GitWorktreeSourceJournalStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree source journal creation request is invalid");
            }
            const auto encoded = details::EncodeGitWorktreeSourceJournal(journal);
            if (!encoded)
            {
                return _failure(
                    GitWorktreeSourceJournalStatus::InvalidRequest,
                    ERROR_INVALID_DATA,
                    "Git Worktree source journal could not be encoded");
            }
            const auto written = details::WriteNewGitWorktreeMetadataFile(
                layout.SnapshotPath,
                *encoded);
            if (!written.Succeeded())
            {
                return _fileFailure(written);
            }
            const auto verified = ReadGitWorktreeSourceJournal(layout, marker);
            if (!verified.Succeeded() ||
                !_sameJournal(*verified.Journal, journal))
            {
                return verified.Succeeded() ?
                           _failure(
                               GitWorktreeSourceJournalStatus::IdentityMismatch,
                               ERROR_INVALID_DATA,
                               "Created Git Worktree source journal verification failed") :
                           verified;
            }
            return verified;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeSourceJournalStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree source journal creation exhausted local resources");
        }
    }
}
