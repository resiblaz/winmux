// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeBaselineInternal.h"

#include "GitSourceCommandOutput.h"

namespace Microsoft::Terminal::TaskGit::details
{
    GitWorktreeBaselineResult GitWorktreeBaselineFailure(
        const GitWorktreeBaselineStatus status,
        const DWORD systemError,
        std::string error)
    {
        GitWorktreeBaselineResult result;
        result.Status = status;
        result.SystemError = systemError;
        result.Error = std::move(error);
        return result;
    }

    GitWorktreeBaselineResult PreserveGitWorktreeBaselineFailure(
        GitWorktreeBaselineResult result,
        const GitWorktreeLayout& layout)
    {
        result.Preserved = true;
        result.PreservedPath = layout.WorktreePathUtf8;
        return result;
    }

    GitWorktreeBaselineStatus MapGitWorktreeBaselineInspectionStatus(
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus;
        switch (status)
        {
        case GitRepositoryInspectionStatus::Cancelled:
            return GitWorktreeBaselineStatus::Cancelled;
        case GitRepositoryInspectionStatus::TimedOut:
            return GitWorktreeBaselineStatus::TimedOut;
        case GitRepositoryInspectionStatus::LockFailed:
            return GitWorktreeBaselineStatus::LockFailed;
        case GitRepositoryInspectionStatus::ResourceFailure:
            return GitWorktreeBaselineStatus::ResourceFailure;
        default:
            return GitWorktreeBaselineStatus::InspectionFailed;
        }
    }

    GitWorktreeBaselineStatus MapGitWorktreeBaselineLockStatus(
        const Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus;
        switch (status)
        {
        case TaskDirectoryLockStatus::Cancelled:
            return GitWorktreeBaselineStatus::Cancelled;
        case TaskDirectoryLockStatus::TimedOut:
            return GitWorktreeBaselineStatus::TimedOut;
        case TaskDirectoryLockStatus::ResourceFailure:
            return GitWorktreeBaselineStatus::ResourceFailure;
        default:
            return GitWorktreeBaselineStatus::LockFailed;
        }
    }

    GitWorktreeBaselineStatus MapGitWorktreeBaselineCommandStatus(
        const Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitCommandStatus;
        switch (status)
        {
        case GitCommandStatus::Cancelled:
            return GitWorktreeBaselineStatus::Cancelled;
        case GitCommandStatus::TimedOut:
            return GitWorktreeBaselineStatus::TimedOut;
        case GitCommandStatus::InvalidRequest:
            return GitWorktreeBaselineStatus::InvalidRequest;
        case GitCommandStatus::ResourceFailure:
            return GitWorktreeBaselineStatus::ResourceFailure;
        default:
            return GitWorktreeBaselineStatus::GitFailure;
        }
    }

    GitWorktreeCommitPolicyResult InspectGitWorktreeCommitPolicy(
        const GitSourceCommandInvoker& invoke) noexcept
    {
        GitWorktreeCommitPolicyResult result;
        try
        {
            if (!invoke)
            {
                result.Failure = GitWorktreeBaselineFailure(
                    GitWorktreeBaselineStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Task Worktree commit policy invoker is unavailable");
                return result;
            }
            const auto name = invoke({ L"config", L"--get", L"user.name" });
            const auto email = invoke({ L"config", L"--get", L"user.email" });
            const auto signing = invoke({
                L"config",
                L"--bool",
                L"--get",
                L"commit.gpgSign",
            });
            if (!name || !email || !signing)
            {
                result.Failure = GitWorktreeBaselineFailure(
                    GitWorktreeBaselineStatus::GitFailure,
                    ERROR_GEN_FAILURE,
                    "Task Worktree commit configuration query is unavailable");
                return result;
            }
            const auto validExit = [](const auto& command) {
                return command.ExitCode == 0 ||
                       (command.ExitCode == 1 && command.Stdout.empty());
            };
            if (!validExit(*name) || !validExit(*email) ||
                !validExit(*signing))
            {
                result.Failure = GitWorktreeBaselineFailure(
                    GitWorktreeBaselineStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Task Worktree commit configuration query failed");
                return result;
            }
            const auto configuredName = name->ExitCode == 0 ?
                                            ParseGitSourceLine(*name) :
                                            std::optional<std::string>{};
            const auto configuredEmail = email->ExitCode == 0 ?
                                             ParseGitSourceLine(*email) :
                                             std::optional<std::string>{};
            if ((name->ExitCode == 0 && !configuredName) ||
                (email->ExitCode == 0 && !configuredEmail))
            {
                result.Failure = GitWorktreeBaselineFailure(
                    GitWorktreeBaselineStatus::IdentityFailure,
                    ERROR_INVALID_DATA,
                    "Task Worktree commit identity configuration is invalid");
                return result;
            }
            if (configuredName.has_value() != configuredEmail.has_value())
            {
                result.Failure = GitWorktreeBaselineFailure(
                    GitWorktreeBaselineStatus::IdentityFailure,
                    ERROR_INVALID_DATA,
                    "Task Worktree commit identity is incomplete");
                return result;
            }
            result.UseFallbackIdentity = !configuredName && !configuredEmail;

            if (signing->ExitCode == 0)
            {
                const auto value = ParseGitSourceLine(*signing);
                if (!value || (*value != "true" && *value != "false"))
                {
                    result.Failure = GitWorktreeBaselineFailure(
                        GitWorktreeBaselineStatus::GitFailure,
                        ERROR_INVALID_DATA,
                        "Task Worktree signing configuration is invalid");
                    return result;
                }
                result.SigningConfigured = *value == "true";
            }
            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            result.Failure = GitWorktreeBaselineFailure(
                GitWorktreeBaselineStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree commit policy exhausted local resources");
            return result;
        }
    }
}
