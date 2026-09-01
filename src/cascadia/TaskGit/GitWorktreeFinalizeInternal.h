// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeFinalize.h"

#include "GitCommandRunner.h"
#include "GitRepositoryLock.h"
#include "GitWorktreePreflight.h"

namespace Microsoft::Terminal::TaskGit::details
{
    inline constexpr size_t GitWorktreeFinalizeStdoutBytes{ 1024 * 1024 };
    inline constexpr size_t GitWorktreeFinalizeStderrBytes{ 64 * 1024 };

    struct GitWorktreeFinalizeCommandContext
    {
        Microsoft::Terminal::TaskExecution::GitRepositoryInspection Source;
        Microsoft::Terminal::TaskExecution::GitRepositoryLock Lock;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor Executor;
        HANDLE CancellationEvent{};
        uint64_t Deadline{};
        GitWorktreeFinalizeStatus FailureStatus{
            GitWorktreeFinalizeStatus::GitFailure
        };
        DWORD FailureSystemError{};
        std::string FailureError;

        [[nodiscard]] std::optional<Microsoft::Terminal::TaskExecution::GitCommandResult>
        Run(
            const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity& directory,
            std::vector<std::wstring> arguments);
        [[nodiscard]] std::optional<Microsoft::Terminal::TaskExecution::GitCommandResult>
        RunSource(std::vector<std::wstring> arguments);
    };

    struct GitWorktreeFinalizeContextResult
    {
        std::optional<GitWorktreeFinalizeCommandContext> Context;
        GitWorktreeFinalizeStatus Status{ GitWorktreeFinalizeStatus::GitFailure };
        DWORD SystemError{};
        std::string Error;
    };

    struct GitWorktreeFinalizeRegistrationResult
    {
        bool Succeeded{};
        std::optional<GitWorktreeRegistration> Managed;
        bool BranchRegisteredElsewhere{};
        std::string Error;
    };

    struct GitWorktreeFinalizeCheckpointResult
    {
        bool Succeeded{};
        std::string FinalCommit;
        std::string CheckpointCommit;
        bool CommitCreated{};
        bool UsedFallbackIdentity{};
        bool SigningConfigured{};
        GitWorktreeFinalizeResult Failure;
    };

    [[nodiscard]] GitWorktreeFinalizeContextResult OpenGitWorktreeFinalizeContext(
        const std::filesystem::path& gitExecutable,
        const Microsoft::Terminal::TaskExecution::GitCommandExecutor& executor,
        const GitWorktreeMarker& marker,
        const std::filesystem::path& repositoryRoot,
        HANDLE cancellationEvent,
        uint32_t timeoutMilliseconds) noexcept;
    [[nodiscard]] GitWorktreeFinalizeStatus MapGitWorktreeFinalizeCommandStatus(
        Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept;
    [[nodiscard]] GitWorktreeFinalizeStatus MapGitWorktreeFinalizeInspectionStatus(
        Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept;
    [[nodiscard]] GitWorktreeFinalizeStatus MapGitWorktreeFinalizeLockStatus(
        Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus status) noexcept;
    [[nodiscard]] std::optional<bool> GitWorktreeFinalizePathExists(
        const std::filesystem::path& path,
        DWORD& systemError) noexcept;
    [[nodiscard]] GitWorktreeFinalizeRegistrationResult
    FindGitWorktreeFinalizeRegistration(
        const GitWorktreeLayout& layout,
        std::span<const GitWorktreeRegistration> registrations) noexcept;
    [[nodiscard]] GitWorktreeFinalizeCheckpointResult
    CheckpointGitWorktreeForFinalization(
        GitWorktreeFinalizeCommandContext& context,
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspection& target) noexcept;
    [[nodiscard]] GitWorktreeFinalizeResult CleanupGitWorktreeFinalizeSidecars(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& expectedMarker) noexcept;
    [[nodiscard]] std::string GitWorktreeFinalizeSubject(
        const GitWorktreeMarker& marker);
}
