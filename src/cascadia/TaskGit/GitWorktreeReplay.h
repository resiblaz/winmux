// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeFileCopy.h"
#include "GitWorktreeProvision.h"

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr uint32_t DefaultGitWorktreeReplayTimeoutMilliseconds{
        180000
    };
    inline constexpr uint32_t MaximumGitWorktreeReplayTimeoutMilliseconds{
        300000
    };

    enum class GitWorktreeReplayStatus
    {
        Succeeded,
        InvalidRequest,
        EvidenceMismatch,
        InspectionFailed,
        LockFailed,
        Cancelled,
        TimedOut,
        PreflightRejected,
        TargetChanged,
        TrackedApplyFailed,
        CopyFailed,
        VerificationFailed,
        GitFailure,
        ResourceFailure,
    };

    struct GitWorktreeReplayResult
    {
        GitWorktreeReplayStatus Status{ GitWorktreeReplayStatus::GitFailure };
        GitWorktreePreflightStatus PreflightStatus{
            GitWorktreePreflightStatus::Succeeded
        };
        GitWorktreeFileCopyStatus FileCopyStatus{
            GitWorktreeFileCopyStatus::Succeeded
        };
        bool TrackedApplied{};
        size_t FilesCopied{};
        size_t FilesReused{};
        bool Preserved{};
        std::string PreservedPath;
        std::string FailurePath;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeReplayStatus::Succeeded;
        }
    };

    class GitWorktreeReplayer final
    {
    public:
        explicit GitWorktreeReplayer(
            std::filesystem::path gitExecutable,
            Microsoft::Terminal::TaskExecution::GitCommandExecutor executor = {});

        // Replay is idempotent only for the frozen snapshot. After durable
        // evidence is accepted, every failure preserves the managed path;
        // unrelated target changes are never overwritten or cleaned up.
        [[nodiscard]] GitWorktreeReplayResult Replay(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker,
            const GitWorktreeSourceJournal& journal,
            HANDLE cancellationEvent,
            uint32_t timeoutMilliseconds =
                DefaultGitWorktreeReplayTimeoutMilliseconds) const noexcept;

    private:
        std::filesystem::path _gitExecutable;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor _executor;
    };
}
