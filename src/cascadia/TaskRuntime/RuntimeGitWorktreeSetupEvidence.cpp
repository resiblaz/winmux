// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeGitWorktreeSetupEvidence.h"

#include "TaskRecordValidation.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    namespace
    {
        std::optional<std::filesystem::path> _widePath(
            const std::string_view value)
        {
            if (value.empty() ||
                value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return std::nullopt;
            }
            const auto required = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0);
            if (required <= 0)
            {
                return std::nullopt;
            }
            std::wstring decoded(static_cast<size_t>(required), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    decoded.data(),
                    required) != required)
            {
                return std::nullopt;
            }
            const std::filesystem::path path{ decoded };
            return path.is_absolute() && path.has_root_path() ?
                       std::optional<std::filesystem::path>{ path.lexically_normal() } :
                       std::nullopt;
        }

        bool _sameMarkerIdentity(
            const Microsoft::Terminal::TaskGit::GitWorktreeMarker& expected,
            const Microsoft::Terminal::TaskGit::GitWorktreeMarker& actual) noexcept
        {
            return expected.FormatVersion == actual.FormatVersion &&
                   expected.Task == actual.Task &&
                   expected.RepositoryIdentityDigest ==
                       actual.RepositoryIdentityDigest &&
                   expected.BranchRef == actual.BranchRef &&
                   expected.OwnershipNonce == actual.OwnershipNonce &&
                   expected.WorktreePath == actual.WorktreePath;
        }
    }

    RuntimeGitWorktreeLayoutEvidenceResult PrepareRuntimeGitWorktreeLayoutEvidence(
        const Microsoft::Terminal::TaskCore::TaskGitWorktreeRecord& record,
        const std::filesystem::path& runtimeDataRoot) noexcept
    {
        RuntimeGitWorktreeLayoutEvidenceResult result;
        try
        {
            if (!Microsoft::Terminal::TaskCore::ValidateTaskGitWorktreeRecord(record)
                     .empty())
            {
                result.SystemError = ERROR_INVALID_DATA;
                result.Error = "Runtime Git Worktree record is invalid";
                return result;
            }
            const auto layout =
                Microsoft::Terminal::TaskGit::PrepareGitWorktreeLayout(
                    runtimeDataRoot,
                    record.Task);
            const auto sourcePath = _widePath(record.RepositoryRoot);
            if (!layout.Succeeded() || !sourcePath ||
                layout.Layout->WorktreePathUtf8 != record.WorktreePath ||
                layout.Layout->BranchRef != record.BranchRef)
            {
                result.SystemError = layout.SystemError == ERROR_SUCCESS ?
                                         ERROR_REVISION_MISMATCH :
                                         layout.SystemError;
                result.Error = "Runtime Git Worktree layout does not match its record";
                return result;
            }
            Microsoft::Terminal::TaskGit::GitWorktreeMarker marker;
            marker.Task = record.Task;
            marker.RepositoryIdentityDigest = record.RepositoryIdentityDigest;
            marker.BranchRef = record.BranchRef;
            marker.SnapshotDigest = record.SnapshotDigest;
            marker.BaselineCommit = record.BaselineCommit;
            marker.OwnershipNonce = record.OwnershipNonce;
            marker.WorktreePath = record.WorktreePath;
            if (!Microsoft::Terminal::TaskGit::ValidateGitWorktreeMarker(
                    *layout.Layout,
                    marker))
            {
                result.SystemError = ERROR_INVALID_DATA;
                result.Error = "Runtime Git Worktree marker identity is invalid";
                return result;
            }
            result.Succeeded = true;
            result.Layout = *layout.Layout;
            result.SourcePath = *sourcePath;
            result.ExpectedMarker = std::move(marker);
            return result;
        }
        catch (...)
        {
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error = "Runtime Git Worktree layout exhausted local resources";
            return result;
        }
    }

    RuntimeGitWorktreeEvidenceResult LoadRuntimeGitWorktreeEvidence(
        const Microsoft::Terminal::TaskCore::TaskGitWorktreeRecord& record,
        const Microsoft::Terminal::TaskGit::GitWorktreeLayout& layout,
        const Microsoft::Terminal::TaskGit::GitWorktreeMarker& expectedMarker) noexcept
    {
        RuntimeGitWorktreeEvidenceResult result;
        try
        {
            const auto marker =
                Microsoft::Terminal::TaskGit::ReadGitWorktreeMarker(layout);
            if (!marker.Succeeded() ||
                !_sameMarkerIdentity(expectedMarker, *marker.Marker) ||
                marker.Marker->SnapshotDigest != record.SnapshotDigest ||
                (!record.BaselineCommit.empty() &&
                 marker.Marker->BaselineCommit != record.BaselineCommit) ||
                (record.State !=
                         Microsoft::Terminal::TaskCore::TaskGitWorktreeState::CreatingBaseline &&
                 record.BaselineCommit.empty() &&
                 !marker.Marker->BaselineCommit.empty()))
            {
                result.SystemError = marker.SystemError == ERROR_SUCCESS ?
                                         ERROR_REVISION_MISMATCH :
                                         marker.SystemError;
                result.Error = "Runtime Git Worktree marker evidence does not match";
                return result;
            }
            const auto journal =
                Microsoft::Terminal::TaskGit::ReadGitWorktreeSourceJournal(
                    layout,
                    *marker.Marker);
            if (!journal.Succeeded() ||
                journal.Journal->SnapshotDigest != record.SnapshotDigest ||
                journal.Journal->SourceHead != record.SourceHead ||
                journal.Journal->SourceReference != record.SourceReference)
            {
                result.SystemError = journal.SystemError == ERROR_SUCCESS ?
                                         ERROR_REVISION_MISMATCH :
                                         journal.SystemError;
                result.Error = "Runtime Git Worktree source journal does not match";
                return result;
            }
            result.Succeeded = true;
            result.Marker = *marker.Marker;
            result.Journal = *journal.Journal;
            return result;
        }
        catch (...)
        {
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error = "Runtime Git Worktree evidence exhausted local resources";
            return result;
        }
    }
}
