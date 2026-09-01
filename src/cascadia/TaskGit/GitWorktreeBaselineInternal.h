// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeBaseline.h"
#include "GitTrackedSource.h"
#include "GitWorktreeReplayProof.h"

#include "GitRepositoryLock.h"

namespace Microsoft::Terminal::TaskGit::details
{
    struct GitWorktreeCommitPolicyResult
    {
        bool Succeeded{};
        bool UseFallbackIdentity{};
        bool SigningConfigured{};
        GitWorktreeBaselineResult Failure;
    };

    struct GitWorktreeBaselineTreeValidationResult
    {
        bool Succeeded{};
        std::string Tree;
        GitWorktreeBaselineResult Failure;
    };

    [[nodiscard]] GitWorktreeBaselineResult GitWorktreeBaselineFailure(
        GitWorktreeBaselineStatus status,
        DWORD systemError,
        std::string error);
    [[nodiscard]] GitWorktreeBaselineResult PreserveGitWorktreeBaselineFailure(
        GitWorktreeBaselineResult result,
        const GitWorktreeLayout& layout);
    [[nodiscard]] GitWorktreeBaselineStatus MapGitWorktreeBaselineInspectionStatus(
        Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept;
    [[nodiscard]] GitWorktreeBaselineStatus MapGitWorktreeBaselineLockStatus(
        Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus status) noexcept;
    [[nodiscard]] GitWorktreeBaselineStatus MapGitWorktreeBaselineCommandStatus(
        Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept;
    [[nodiscard]] GitWorktreeCommitPolicyResult InspectGitWorktreeCommitPolicy(
        const GitSourceCommandInvoker& invoke) noexcept;
    [[nodiscard]] GitWorktreeBaselineTreeValidationResult
    ValidateGitWorktreeStagedBaseline(
        const GitWorktreeReplayProof& proof,
        const GitWorktreeSourceJournal& journal,
        const GitSourceCommandInvoker& invoke,
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            targetRoot,
        HANDLE cancellationEvent) noexcept;
    [[nodiscard]] GitWorktreeBaselineTreeValidationResult
    ValidateGitWorktreeCommittedBaseline(
        std::string_view commit,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal,
        const GitSourceCommandInvoker& invoke,
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            targetRoot,
        HANDLE cancellationEvent) noexcept;
}
