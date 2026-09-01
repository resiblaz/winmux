// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeSourceJournal.h"

#include "GitRepositoryInspector.h"
#include "TaskIdentifiers.h"

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr uint32_t DefaultGitWorktreeCheckpointTimeoutMilliseconds{
        120000
    };
    inline constexpr uint32_t MaximumGitWorktreeCheckpointTimeoutMilliseconds{
        300000
    };
    inline constexpr size_t MaximumGitWorktreeCheckpointChangedPaths{ 2000 };
    inline constexpr size_t MaximumGitWorktreeCheckpointJsonBytes{ 64 * 1024 };

    enum class GitWorktreeCheckpointStatus
    {
        Succeeded,
        AlreadyCommitted,
        InvalidRequest,
        EvidenceMismatch,
        InspectionFailed,
        LockFailed,
        Cancelled,
        TimedOut,
        TargetChanged,
        OutputLimitExceeded,
        IdentityFailure,
        SigningFailure,
        GitFailure,
        VerificationFailed,
        ResourceFailure,
    };

    struct GitWorktreeCheckpointStartResult
    {
        GitWorktreeCheckpointStatus Status{
            GitWorktreeCheckpointStatus::GitFailure
        };
        std::string StartCommit;
        std::string StateDigest;
        std::string DiffSummaryJson;
        bool Preserved{};
        std::string PreservedPath;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeCheckpointStatus::Succeeded &&
                   !StartCommit.empty() && !StateDigest.empty() &&
                   !DiffSummaryJson.empty();
        }
    };

    struct GitWorktreeCheckpointInspectionResult
    {
        GitWorktreeCheckpointStatus Status{
            GitWorktreeCheckpointStatus::GitFailure
        };
        std::string AgentEndCommit;
        std::string ChangedPathsJson;
        std::string DiffSummaryJson;
        bool NeedsCommit{};
        bool Preserved{};
        std::string PreservedPath;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeCheckpointStatus::Succeeded &&
                   !AgentEndCommit.empty() && !ChangedPathsJson.empty() &&
                   !DiffSummaryJson.empty();
        }
    };

    struct GitWorktreeCheckpointCommitResult
    {
        GitWorktreeCheckpointStatus Status{
            GitWorktreeCheckpointStatus::GitFailure
        };
        std::string CheckpointCommit;
        bool CommitCreated{};
        bool UsedFallbackIdentity{};
        bool SigningConfigured{};
        bool Preserved{};
        std::string PreservedPath;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return (Status == GitWorktreeCheckpointStatus::Succeeded ||
                    Status == GitWorktreeCheckpointStatus::AlreadyCommitted) &&
                   !CheckpointCommit.empty();
        }
    };

    class GitWorktreeCheckpointManager final
    {
    public:
        explicit GitWorktreeCheckpointManager(
            std::filesystem::path gitExecutable,
            Microsoft::Terminal::TaskExecution::GitCommandExecutor executor = {});

        [[nodiscard]] GitWorktreeCheckpointStartResult CaptureStart(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker,
            const GitWorktreeSourceJournal& journal,
            HANDLE cancellationEvent,
            uint32_t timeoutMilliseconds =
                DefaultGitWorktreeCheckpointTimeoutMilliseconds) const noexcept;

        [[nodiscard]] GitWorktreeCheckpointInspectionResult InspectAndStage(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker,
            const GitWorktreeSourceJournal& journal,
            std::string_view startCommit,
            std::string_view startStateDigest,
            HANDLE cancellationEvent,
            uint32_t timeoutMilliseconds =
                DefaultGitWorktreeCheckpointTimeoutMilliseconds) const noexcept;

        [[nodiscard]] GitWorktreeCheckpointCommitResult CommitOrRecover(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker,
            const GitWorktreeSourceJournal& journal,
            const Microsoft::Terminal::TaskCore::TaskRunId& run,
            std::string_view agentEndCommit,
            HANDLE cancellationEvent,
            uint32_t timeoutMilliseconds =
                DefaultGitWorktreeCheckpointTimeoutMilliseconds) const noexcept;

    private:
        std::filesystem::path _gitExecutable;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor _executor;
    };

    [[nodiscard]] std::optional<std::string>
    ParseGitWorktreeCheckpointStartDigest(std::string_view summaryJson) noexcept;
}
