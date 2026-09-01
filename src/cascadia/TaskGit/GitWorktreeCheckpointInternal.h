// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeCheckpoint.h"

#include "GitCommandRunner.h"
#include "GitRepositoryLock.h"

namespace Microsoft::Terminal::TaskGit::details
{
    inline constexpr size_t GitWorktreeCheckpointStdoutBytes{ 512 * 1024 };
    inline constexpr size_t GitWorktreeCheckpointStderrBytes{ 64 * 1024 };

    struct GitWorktreeCheckpointCommandContext
    {
        Microsoft::Terminal::TaskExecution::GitRepositoryInspection Target;
        Microsoft::Terminal::TaskExecution::GitRepositoryLock Lock;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor Executor;
        HANDLE CancellationEvent{};
        uint64_t Deadline{};
        GitWorktreeCheckpointStatus FailureStatus{
            GitWorktreeCheckpointStatus::GitFailure
        };
        DWORD FailureSystemError{};
        std::string FailureError;

        [[nodiscard]] std::optional<Microsoft::Terminal::TaskExecution::GitCommandResult>
        Run(std::vector<std::wstring> arguments);
    };

    struct GitWorktreeCheckpointContextResult
    {
        std::optional<GitWorktreeCheckpointCommandContext> Context;
        GitWorktreeCheckpointStatus Status{
            GitWorktreeCheckpointStatus::GitFailure
        };
        DWORD SystemError{};
        std::string Error;
    };

    struct GitWorktreeCheckpointDiffSummary
    {
        uint64_t Files{};
        uint64_t Additions{};
        uint64_t Deletions{};
        uint64_t BinaryFiles{};
    };

    [[nodiscard]] GitWorktreeCheckpointContextResult
    OpenGitWorktreeCheckpointContext(
        const std::filesystem::path& gitExecutable,
        const Microsoft::Terminal::TaskExecution::GitCommandExecutor& executor,
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal,
        HANDLE cancellationEvent,
        uint32_t timeoutMilliseconds) noexcept;
    [[nodiscard]] GitWorktreeCheckpointStatus MapGitWorktreeCheckpointCommandStatus(
        Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept;
    [[nodiscard]] GitWorktreeCheckpointStatus MapGitWorktreeCheckpointInspectionStatus(
        Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept;
    [[nodiscard]] GitWorktreeCheckpointStatus MapGitWorktreeCheckpointLockStatus(
        Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus status) noexcept;
    [[nodiscard]] std::optional<GitWorktreeCheckpointDiffSummary>
    ParseGitWorktreeCheckpointNumstat(std::string_view output) noexcept;
    [[nodiscard]] std::optional<std::string>
    EncodeGitWorktreeCheckpointPaths(std::span<const std::string> paths) noexcept;
    [[nodiscard]] std::optional<std::string>
    EncodeGitWorktreeCheckpointSummary(
        std::string_view startDigest,
        std::string_view endDigest,
        const GitWorktreeCheckpointDiffSummary& summary) noexcept;
    [[nodiscard]] std::string GitWorktreeCheckpointSubject(
        const Microsoft::Terminal::TaskCore::TaskRunId& run);
    [[nodiscard]] bool VerifyGitWorktreeCheckpointCommit(
        GitWorktreeCheckpointCommandContext& context,
        const GitWorktreeLayout& layout,
        const Microsoft::Terminal::TaskCore::TaskRunId& run,
        std::string_view agentEndCommit,
        std::string_view checkpointCommit,
        Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat,
        std::string& error) noexcept;
}
