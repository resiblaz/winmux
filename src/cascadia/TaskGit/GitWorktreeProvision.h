// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreePreflight.h"

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr uint32_t DefaultGitWorktreeProvisionTimeoutMilliseconds{
        180000
    };
    inline constexpr uint32_t MaximumGitWorktreeProvisionTimeoutMilliseconds{
        300000
    };

    enum class GitWorktreeProvisionStatus
    {
        Succeeded,
        InvalidRequest,
        InspectionFailed,
        LockFailed,
        Cancelled,
        TimedOut,
        PreflightRejected,
        GitFailure,
        VerificationFailed,
        ResourceFailure,
    };

    struct GitWorktreeProvisionResult
    {
        GitWorktreeProvisionStatus Status{
            GitWorktreeProvisionStatus::GitFailure
        };
        GitWorktreePreflightStatus PreflightStatus{
            GitWorktreePreflightStatus::Succeeded
        };
        GitWorktreePreflightDisposition InitialDisposition{
            GitWorktreePreflightDisposition::None
        };
        bool WorktreeCreated{};
        bool BranchCreated{};
        bool Preserved{};
        std::string PreservedPath;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeProvisionStatus::Succeeded &&
                   InitialDisposition !=
                       GitWorktreePreflightDisposition::None;
        }
    };

    class GitWorktreeProvisioner final
    {
    public:
        explicit GitWorktreeProvisioner(
            std::filesystem::path gitExecutable,
            Microsoft::Terminal::TaskExecution::GitCommandExecutor executor = {});

        // The caller must persist and reload matching marker/journal evidence
        // before entry. Failures after Git may have created state are reported
        // as Preserved; this operation never prunes, force-removes, or invents
        // a fallback branch name.
        [[nodiscard]] GitWorktreeProvisionResult Provision(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker,
            const GitWorktreeSourceJournal& journal,
            HANDLE cancellationEvent,
            GitWorktreePreflightMode mode,
            uint32_t timeoutMilliseconds =
                DefaultGitWorktreeProvisionTimeoutMilliseconds) const noexcept;

    private:
        std::filesystem::path _gitExecutable;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor _executor;
    };
}
