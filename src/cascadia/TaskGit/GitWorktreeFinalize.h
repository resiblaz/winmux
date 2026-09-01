// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeSourceJournal.h"

#include "GitRepositoryInspector.h"

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr uint32_t DefaultGitWorktreeFinalizeTimeoutMilliseconds{
        180000
    };
    inline constexpr uint32_t MaximumGitWorktreeFinalizeTimeoutMilliseconds{
        300000
    };

    enum class GitWorktreeFinalizeStatus
    {
        Succeeded,
        AlreadyFinalized,
        InvalidRequest,
        EvidenceMismatch,
        InspectionFailed,
        LockFailed,
        Cancelled,
        TimedOut,
        TargetChanged,
        IdentityFailure,
        SigningFailure,
        GitFailure,
        VerificationFailed,
        SidecarCleanupFailed,
        ResourceFailure,
    };

    struct GitWorktreeFinalizeResult
    {
        GitWorktreeFinalizeStatus Status{
            GitWorktreeFinalizeStatus::GitFailure
        };
        std::string FinalCommit;
        std::string CheckpointCommit;
        bool CheckpointCreated{};
        bool UsedFallbackIdentity{};
        bool SigningConfigured{};
        bool ProducedWork{};
        bool BranchRetained{};
        bool WorktreeRemoved{};
        bool BranchDeleted{};
        bool SidecarsRemoved{};
        bool Preserved{};
        std::string PreservedPath;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return (Status == GitWorktreeFinalizeStatus::Succeeded ||
                    Status == GitWorktreeFinalizeStatus::AlreadyFinalized) &&
                   WorktreeRemoved && SidecarsRemoved &&
                   (BranchRetained != BranchDeleted);
        }
    };

    class GitWorktreeFinalizer final
    {
    public:
        explicit GitWorktreeFinalizer(
            std::filesystem::path gitExecutable,
            Microsoft::Terminal::TaskExecution::GitCommandExecutor executor = {});

        [[nodiscard]] GitWorktreeFinalizeResult Finalize(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& expectedMarker,
            const std::optional<GitWorktreeSourceJournal>& journal,
            const std::filesystem::path& repositoryRoot,
            bool expectedUndeliveredWork,
            HANDLE cancellationEvent,
            uint32_t timeoutMilliseconds =
                DefaultGitWorktreeFinalizeTimeoutMilliseconds) const noexcept;

    private:
        std::filesystem::path _gitExecutable;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor _executor;
    };
}
