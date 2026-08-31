// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitSourceSnapshot.h"

#include "GitCommandTypes.h"
#include "GitRepositoryInspector.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskGit::details
{
    struct GitTrackedSourceProof
    {
        std::string SnapshotCommit;
        std::string WorktreeTree;
        std::string IndexTree;
    };

    enum class GitTrackedSourceProofStatus
    {
        Succeeded,
        CommandUnavailable,
        GitFailure,
        InvalidOutput,
    };

    struct GitTrackedSourceProofResult
    {
        GitTrackedSourceProofStatus Status{
            GitTrackedSourceProofStatus::InvalidOutput
        };
        std::optional<GitTrackedSourceProof> Proof;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitTrackedSourceProofStatus::Succeeded &&
                   Proof.has_value();
        }
    };

    using GitSourceCommandInvoker = std::function<std::optional<
        Microsoft::Terminal::TaskExecution::GitCommandResult>(
        std::vector<std::wstring>)>;

    [[nodiscard]] GitTrackedSourceProofResult CaptureGitTrackedSourceProof(
        const GitSourceStatusSummary& summary,
        Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat,
        const GitSourceCommandInvoker& invoke);
}
