// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeCaptureJournal.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        GitWorktreeCaptureJournalResult _failure(
            const GitWorktreeCaptureJournalStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreeCaptureJournalResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _sameMarker(
            const GitWorktreeMarker& expected,
            const GitWorktreeMarker& actual,
            const bool requireSnapshot) noexcept
        {
            return expected.FormatVersion == actual.FormatVersion &&
                   expected.Task == actual.Task &&
                   expected.RepositoryIdentityDigest ==
                       actual.RepositoryIdentityDigest &&
                   expected.BranchRef == actual.BranchRef &&
                   expected.BaselineCommit == actual.BaselineCommit &&
                   expected.OwnershipNonce == actual.OwnershipNonce &&
                   expected.WorktreePath == actual.WorktreePath &&
                   (!requireSnapshot || !actual.SnapshotDigest.empty()) &&
                   (expected.SnapshotDigest.empty() ||
                    expected.SnapshotDigest == actual.SnapshotDigest);
        }

        GitWorktreeCaptureJournalResult _success(
            GitWorktreeMarker marker,
            GitWorktreeSourceJournal journal,
            const bool captured,
            const bool recoveredMarker)
        {
            GitWorktreeCaptureJournalResult result;
            result.Status = GitWorktreeCaptureJournalStatus::Succeeded;
            result.Marker = std::move(marker);
            result.Journal = std::move(journal);
            result.Captured = captured;
            result.RecoveredMarker = recoveredMarker;
            return result;
        }
    }

    GitWorktreeCaptureJournal::GitWorktreeCaptureJournal(
        std::filesystem::path gitExecutable,
        Microsoft::Terminal::TaskExecution::GitCommandExecutor executor,
        GitSnapshotFileHasher fileHasher) :
        _gitExecutable{ std::move(gitExecutable) },
        _executor{ std::move(executor) },
        _fileHasher{ std::move(fileHasher) }
    {
    }

    GitWorktreeCaptureJournalResult GitWorktreeCaptureJournal::CaptureOrLoad(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& expectedMarker,
        const std::filesystem::path& sourcePath,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) const noexcept
    {
        try
        {
            if (_gitExecutable.empty() || sourcePath.empty() ||
                !cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE ||
                timeoutMilliseconds == 0 ||
                timeoutMilliseconds > MaximumGitSourceCaptureTimeoutMilliseconds ||
                !ValidateGitWorktreeMarker(layout, expectedMarker) ||
                !expectedMarker.SnapshotDigest.empty() ||
                !expectedMarker.BaselineCommit.empty())
            {
                return _failure(
                    GitWorktreeCaptureJournalStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree capture journal request is invalid");
            }

            const auto marker = ReadGitWorktreeMarker(layout);
            if (marker.Succeeded())
            {
                if (!_sameMarker(expectedMarker, *marker.Marker, false))
                {
                    return _failure(
                        GitWorktreeCaptureJournalStatus::MarkerFailure,
                        ERROR_REVISION_MISMATCH,
                        "Persisted Git Worktree marker does not match capture ownership");
                }
                if (marker.Marker->SnapshotDigest.empty())
                {
                    return _failure(
                        GitWorktreeCaptureJournalStatus::EvidenceIncomplete,
                        ERROR_INVALID_DATA,
                        "Persisted Git Worktree marker has no snapshot digest");
                }
                const auto journal = ReadGitWorktreeSourceJournal(
                    layout,
                    *marker.Marker);
                if (!journal.Succeeded())
                {
                    return _failure(
                        journal.Status ==
                                GitWorktreeSourceJournalStatus::ResourceFailure ?
                            GitWorktreeCaptureJournalStatus::ResourceFailure :
                            GitWorktreeCaptureJournalStatus::EvidenceIncomplete,
                        journal.SystemError,
                        "Persisted Git Worktree marker has no valid source journal");
                }
                return _success(
                    *marker.Marker,
                    *journal.Journal,
                    false,
                    false);
            }
            if (marker.Status != GitWorktreeMarkerStatus::Missing)
            {
                return _failure(
                    marker.Status == GitWorktreeMarkerStatus::ResourceFailure ?
                        GitWorktreeCaptureJournalStatus::ResourceFailure :
                        GitWorktreeCaptureJournalStatus::MarkerFailure,
                    marker.SystemError,
                    "Git Worktree capture marker could not be inspected");
            }

            const auto existingJournal = ReadGitWorktreeSourceJournal(
                layout,
                expectedMarker);
            if (existingJournal.Succeeded())
            {
                auto recovered = expectedMarker;
                recovered.SnapshotDigest =
                    existingJournal.Journal->SnapshotDigest;
                const auto createdMarker = CreateGitWorktreeMarker(
                    layout,
                    recovered);
                if (!createdMarker.Succeeded())
                {
                    return _failure(
                        createdMarker.Status ==
                                GitWorktreeMarkerStatus::ResourceFailure ?
                            GitWorktreeCaptureJournalStatus::ResourceFailure :
                            GitWorktreeCaptureJournalStatus::MarkerFailure,
                        createdMarker.SystemError,
                        "Git Worktree capture marker recovery failed");
                }
                return _success(
                    *createdMarker.Marker,
                    *existingJournal.Journal,
                    false,
                    true);
            }
            if (existingJournal.Status != GitWorktreeSourceJournalStatus::Missing)
            {
                return _failure(
                    existingJournal.Status ==
                            GitWorktreeSourceJournalStatus::ResourceFailure ?
                        GitWorktreeCaptureJournalStatus::ResourceFailure :
                        GitWorktreeCaptureJournalStatus::JournalFailure,
                    existingJournal.SystemError,
                    "Git Worktree source journal could not be inspected");
            }

            GitSourceCapture capture{
                _gitExecutable,
                _executor,
                _fileHasher
            };
            const auto snapshot = capture.Capture(
                sourcePath,
                cancellationEvent,
                timeoutMilliseconds);
            if (!snapshot.Succeeded())
            {
                auto result = _failure(
                    snapshot.Status == GitSourceCaptureStatus::ResourceFailure ?
                        GitWorktreeCaptureJournalStatus::ResourceFailure :
                        GitWorktreeCaptureJournalStatus::CaptureFailed,
                    snapshot.SystemError,
                    snapshot.Error);
                result.CaptureStatus = snapshot.Status;
                return result;
            }
            const auto made = MakeGitWorktreeSourceJournal(
                layout,
                expectedMarker,
                *snapshot.Snapshot);
            if (!made.Succeeded())
            {
                return _failure(
                    made.Status == GitWorktreeSourceJournalStatus::ResourceFailure ?
                        GitWorktreeCaptureJournalStatus::ResourceFailure :
                        GitWorktreeCaptureJournalStatus::JournalFailure,
                    made.SystemError,
                    made.Error);
            }
            auto persistedJournal = CreateGitWorktreeSourceJournal(
                layout,
                expectedMarker,
                *made.Journal);
            if (!persistedJournal.Succeeded() &&
                persistedJournal.Status ==
                    GitWorktreeSourceJournalStatus::Collision)
            {
                persistedJournal = ReadGitWorktreeSourceJournal(
                    layout,
                    expectedMarker);
            }
            if (!persistedJournal.Succeeded())
            {
                return _failure(
                    persistedJournal.Status ==
                            GitWorktreeSourceJournalStatus::ResourceFailure ?
                        GitWorktreeCaptureJournalStatus::ResourceFailure :
                        GitWorktreeCaptureJournalStatus::JournalFailure,
                    persistedJournal.SystemError,
                    "Captured Git Worktree source journal could not be persisted");
            }

            auto capturedMarker = expectedMarker;
            capturedMarker.SnapshotDigest =
                persistedJournal.Journal->SnapshotDigest;
            auto persistedMarker = CreateGitWorktreeMarker(
                layout,
                capturedMarker);
            if (!persistedMarker.Succeeded() &&
                persistedMarker.Status == GitWorktreeMarkerStatus::Collision)
            {
                persistedMarker = ReadGitWorktreeMarker(layout);
            }
            if (!persistedMarker.Succeeded() ||
                !_sameMarker(capturedMarker, *persistedMarker.Marker, true) ||
                persistedMarker.Marker->SnapshotDigest !=
                    persistedJournal.Journal->SnapshotDigest)
            {
                return _failure(
                    persistedMarker.Status == GitWorktreeMarkerStatus::ResourceFailure ?
                        GitWorktreeCaptureJournalStatus::ResourceFailure :
                        GitWorktreeCaptureJournalStatus::MarkerFailure,
                    persistedMarker.SystemError == ERROR_SUCCESS ?
                        ERROR_REVISION_MISMATCH :
                        persistedMarker.SystemError,
                    "Captured Git Worktree marker could not be persisted exactly");
            }
            return _success(
                *persistedMarker.Marker,
                *persistedJournal.Journal,
                true,
                false);
        }
        catch (...)
        {
            return _failure(
                GitWorktreeCaptureJournalStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree capture journal exhausted local resources");
        }
    }
}
