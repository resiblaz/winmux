// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitTrackedSource.h"
#include "GitWorktreeSourceJournal.h"

namespace Microsoft::Terminal::TaskGit::details
{
    struct GitWorktreeReplayProof
    {
        GitSourceStatusSummary Status;
        GitTrackedSourceProof Tracked;
        std::vector<std::string> UntrackedPaths;
    };

    enum class GitWorktreeReplayProofStatus
    {
        Succeeded,
        CommandUnavailable,
        GitFailure,
        InvalidOutput,
        Changed,
        ResourceFailure,
    };

    struct GitWorktreeReplayProofResult
    {
        GitWorktreeReplayProofStatus Status{
            GitWorktreeReplayProofStatus::InvalidOutput
        };
        std::optional<GitWorktreeReplayProof> Proof;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeReplayProofStatus::Succeeded &&
                   Proof.has_value();
        }
    };

    [[nodiscard]] GitWorktreeReplayProofResult CaptureGitWorktreeReplayProof(
        Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat,
        const GitSourceCommandInvoker& invoke) noexcept;
    [[nodiscard]] bool GitWorktreeReplayTrackedMatches(
        const GitWorktreeReplayProof& proof,
        const GitWorktreeSourceJournal& journal) noexcept;
    [[nodiscard]] bool GitWorktreeReplayTrackedIsPristine(
        const GitWorktreeReplayProof& proof,
        std::string_view headTree) noexcept;
    [[nodiscard]] bool GitWorktreeReplayManifestMatches(
        const GitWorktreeReplayProof& proof,
        const GitWorktreeSourceJournal& journal) noexcept;
}
