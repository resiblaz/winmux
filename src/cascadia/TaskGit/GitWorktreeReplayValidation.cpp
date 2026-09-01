// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeReplayInternal.h"

#include "GitSnapshotFile.h"
#include "GitSourceCapturePolicy.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        GitWorktreeReplayResult _failure(
            const GitWorktreeReplayStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreeReplayResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }
    }

    GitWorktreeReplayResult ValidateGitWorktreeReplayEvidence(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal) noexcept
    {
        try
        {
            if (!ValidateGitWorktreeSourceJournal(layout, marker, journal))
            {
                return _failure(
                    GitWorktreeReplayStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Task Worktree replay evidence request is invalid");
            }
            const auto persistedMarker = ReadGitWorktreeMarker(layout);
            if (!persistedMarker.Succeeded())
            {
                return _failure(
                    persistedMarker.Status ==
                            GitWorktreeMarkerStatus::ResourceFailure ?
                        GitWorktreeReplayStatus::ResourceFailure :
                        GitWorktreeReplayStatus::EvidenceMismatch,
                    persistedMarker.SystemError,
                    "Task Worktree replay ownership marker is unavailable");
            }
            const auto persistedJournal = ReadGitWorktreeSourceJournal(
                layout,
                *persistedMarker.Marker);
            const auto expectedMarkerBytes = EncodeGitWorktreeMarker(marker);
            const auto actualMarkerBytes = EncodeGitWorktreeMarker(
                *persistedMarker.Marker);
            const auto expectedJournalBytes = EncodeGitWorktreeSourceJournal(
                journal);
            const auto actualJournalBytes = persistedJournal.Succeeded() ?
                                                EncodeGitWorktreeSourceJournal(
                                                    *persistedJournal.Journal) :
                                                std::optional<std::vector<uint8_t>>{};
            if (!persistedJournal.Succeeded() || !expectedMarkerBytes ||
                !actualMarkerBytes || !expectedJournalBytes ||
                !actualJournalBytes ||
                *expectedMarkerBytes != *actualMarkerBytes ||
                *expectedJournalBytes != *actualJournalBytes)
            {
                return _failure(
                    persistedJournal.Status ==
                            GitWorktreeSourceJournalStatus::ResourceFailure ?
                        GitWorktreeReplayStatus::ResourceFailure :
                        GitWorktreeReplayStatus::EvidenceMismatch,
                    persistedJournal.SystemError == ERROR_SUCCESS ?
                        ERROR_REVISION_MISMATCH :
                        persistedJournal.SystemError,
                    "Task Worktree replay durable evidence does not match");
            }
            GitWorktreeReplayResult result;
            result.Status = GitWorktreeReplayStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeReplayStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree replay evidence validation exhausted local resources");
        }
    }

    GitWorktreeReplayResult VerifyGitWorktreeReplayFiles(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            destinationRoot,
        const GitWorktreeSourceJournal& journal,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            for (const auto& expected : journal.UntrackedFiles)
            {
                const auto actual = HashGitSnapshotFile(
                    destinationRoot,
                    expected.RelativePath,
                    cancellationEvent);
                if (!actual.Succeeded())
                {
                    auto result = _failure(
                        actual.Status == GitSnapshotFileStatus::Cancelled ?
                            GitWorktreeReplayStatus::Cancelled :
                        actual.Status == GitSnapshotFileStatus::ResourceFailure ?
                            GitWorktreeReplayStatus::ResourceFailure :
                            GitWorktreeReplayStatus::VerificationFailed,
                        actual.SystemError,
                        "Task Worktree replayed file could not be verified");
                    result.FailurePath = expected.RelativePath;
                    return result;
                }
                if (!ValidateGitSourceFileRecord(
                        *actual.File,
                        expected.RelativePath) ||
                    !SameGitSourceFile(*actual.File, expected))
                {
                    auto result = _failure(
                        GitWorktreeReplayStatus::VerificationFailed,
                        ERROR_CRC,
                        "Task Worktree replayed file differs from its manifest");
                    result.FailurePath = expected.RelativePath;
                    return result;
                }
            }
            GitWorktreeReplayResult result;
            result.Status = GitWorktreeReplayStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeReplayStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree replayed-file verification exhausted local resources");
        }
    }
}
