// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreePreflight.h"

namespace Microsoft::Terminal::TaskGit::details
{
    struct GitWorktreePreflightCommandInvocation
    {
        std::optional<Microsoft::Terminal::TaskExecution::GitCommandResult> Result;
        GitWorktreePreflightResult Failure;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Result.has_value();
        }
    };

    using GitWorktreePreflightCommandInvoker = std::function<
        GitWorktreePreflightCommandInvocation(std::vector<std::wstring>)>;

    enum class GitManagedWorktreePathState
    {
        Absent,
        SafeDirectory,
    };

    [[nodiscard]] GitWorktreePreflightResult EvaluateGitWorktreeCollision(
        const GitWorktreeLayout& layout,
        const GitWorktreeSourceJournal& journal,
        std::span<const GitWorktreeRegistration> registrations,
        const std::optional<std::string>& branchHead,
        GitManagedWorktreePathState pathState,
        GitWorktreePreflightMode mode) noexcept;
    [[nodiscard]] std::optional<GitWorktreePreflightResult>
    CheckGitWorktreeRepositorySupport(
        const GitWorktreeSourceJournal& journal,
        const GitWorktreePreflightCommandInvoker& invoke) noexcept;
    [[nodiscard]] GitWorktreePreflightResult InspectGitWorktreeCollision(
        const GitWorktreeLayout& layout,
        const GitWorktreeSourceJournal& journal,
        GitWorktreePreflightMode mode,
        const GitWorktreePreflightCommandInvoker& invoke) noexcept;
}
