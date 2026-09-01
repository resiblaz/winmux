// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitSourceCapture.h"
#include "GitWorktreeSourceJournal.h"

namespace Microsoft::Terminal::TaskGit
{
    enum class GitWorktreeCaptureJournalStatus
    {
        Succeeded,
        InvalidRequest,
        CaptureFailed,
        EvidenceIncomplete,
        JournalFailure,
        MarkerFailure,
        ResourceFailure,
    };

    struct GitWorktreeCaptureJournalResult
    {
        GitWorktreeCaptureJournalStatus Status{
            GitWorktreeCaptureJournalStatus::JournalFailure
        };
        std::optional<GitWorktreeMarker> Marker;
        std::optional<GitWorktreeSourceJournal> Journal;
        GitSourceCaptureStatus CaptureStatus{
            GitSourceCaptureStatus::GitFailure
        };
        bool Captured{};
        bool RecoveredMarker{};
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeCaptureJournalStatus::Succeeded &&
                   Marker.has_value() && Journal.has_value();
        }
    };

    class GitWorktreeCaptureJournal final
    {
    public:
        explicit GitWorktreeCaptureJournal(
            std::filesystem::path gitExecutable,
            Microsoft::Terminal::TaskExecution::GitCommandExecutor executor = {},
            GitSnapshotFileHasher fileHasher = {});

        [[nodiscard]] GitWorktreeCaptureJournalResult CaptureOrLoad(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& expectedMarker,
            const std::filesystem::path& sourcePath,
            HANDLE cancellationEvent,
            uint32_t timeoutMilliseconds =
                DefaultGitSourceCaptureTimeoutMilliseconds) const noexcept;

    private:
        std::filesystem::path _gitExecutable;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor _executor;
        GitSnapshotFileHasher _fileHasher;
    };
}
