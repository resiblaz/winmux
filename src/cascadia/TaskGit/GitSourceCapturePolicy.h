// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitSourceCapture.h"
#include "GitTrackedSource.h"

namespace Microsoft::Terminal::TaskGit::details
{
    [[nodiscard]] GitSourceCaptureResult GitSourceCaptureFailure(
        GitSourceCaptureStatus status,
        DWORD systemError,
        std::string error);
    [[nodiscard]] GitSourceCaptureStatus MapGitInspectionFailure(
        Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept;
    [[nodiscard]] GitSourceCaptureStatus MapGitCommandFailure(
        Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept;
    [[nodiscard]] GitSourceCaptureStatus MapGitSnapshotFileFailure(
        GitSnapshotFileStatus status) noexcept;
    [[nodiscard]] bool SameGitSourceRepository(
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspection& left,
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspection& right) noexcept;
    [[nodiscard]] bool SameGitSourceFile(
        const GitSourceFileRecord& left,
        const GitSourceFileRecord& right) noexcept;
    [[nodiscard]] bool ValidateGitSourceFileRecord(
        const GitSourceFileRecord& file,
        std::string_view expectedPath) noexcept;
    [[nodiscard]] GitSourceCaptureResult MapGitTrackedProofFailure(
        const GitTrackedSourceProofResult& proof,
        const GitSourceCaptureResult& commandFailure);
}
