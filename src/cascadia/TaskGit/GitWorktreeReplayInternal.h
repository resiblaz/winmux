// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeReplay.h"
#include "GitWorktreeReplayProof.h"

#include "GitRepositoryLock.h"

namespace Microsoft::Terminal::TaskGit::details
{
    [[nodiscard]] GitWorktreeReplayResult GitWorktreeReplayFailure(
        GitWorktreeReplayStatus status,
        DWORD systemError,
        std::string error);
    [[nodiscard]] GitWorktreeReplayResult PreserveGitWorktreeReplayFailure(
        GitWorktreeReplayResult result,
        const GitWorktreeLayout& layout,
        bool trackedApplied = false,
        size_t copied = 0,
        size_t reused = 0);
    [[nodiscard]] GitWorktreeReplayStatus MapGitWorktreeReplayInspectionStatus(
        Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept;
    [[nodiscard]] GitWorktreeReplayStatus MapGitWorktreeReplayLockStatus(
        Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus status) noexcept;
    [[nodiscard]] GitWorktreeReplayStatus MapGitWorktreeReplayCommandStatus(
        Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept;
    [[nodiscard]] GitWorktreeReplayStatus MapGitWorktreeReplayProofStatus(
        GitWorktreeReplayProofStatus status) noexcept;
    [[nodiscard]] GitWorktreePreflightStatus MapGitWorktreeReplayPreflightCommandStatus(
        GitWorktreeReplayStatus status) noexcept;
    [[nodiscard]] std::optional<std::filesystem::path>
    DecodeGitWorktreeReplayPath(std::string_view value);
    [[nodiscard]] GitWorktreeReplayResult GitWorktreeReplayCopyFailure(
        const GitWorktreeFileCopyResult& copy,
        const GitWorktreeLayout& layout,
        const std::string& path,
        bool trackedApplied,
        size_t copied,
        size_t reused);
    [[nodiscard]] GitWorktreeReplayResult GitWorktreeReplayProofFailure(
        const GitWorktreeReplayProofResult& proof,
        const GitWorktreeReplayResult& commandFailure);
    [[nodiscard]] GitWorktreeReplayResult ValidateGitWorktreeReplayEvidence(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal) noexcept;
    [[nodiscard]] GitWorktreeReplayResult VerifyGitWorktreeReplayFiles(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            destinationRoot,
        const GitWorktreeSourceJournal& journal,
        HANDLE cancellationEvent) noexcept;
}
