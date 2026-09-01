// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeReplay.h"

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr uint32_t DefaultGitWorktreeBaselineTimeoutMilliseconds{
        120000
    };
    inline constexpr uint32_t MaximumGitWorktreeBaselineTimeoutMilliseconds{
        300000
    };

    enum class GitWorktreeBaselineStatus
    {
        Succeeded,
        AlreadyCreated,
        InvalidRequest,
        EvidenceMismatch,
        InspectionFailed,
        LockFailed,
        Cancelled,
        TimedOut,
        TargetChanged,
        IdentityFailure,
        SigningFailure,
        GitFailure,
        VerificationFailed,
        MarkerUpdateFailed,
        ResourceFailure,
    };

    struct GitWorktreeBaselineResult
    {
        GitWorktreeBaselineStatus Status{
            GitWorktreeBaselineStatus::GitFailure
        };
        std::string BaselineCommit;
        std::optional<GitWorktreeMarker> Marker;
        bool CommitCreated{};
        bool UsedFallbackIdentity{};
        bool SigningConfigured{};
        bool Preserved{};
        std::string PreservedPath;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return (Status == GitWorktreeBaselineStatus::Succeeded ||
                    Status == GitWorktreeBaselineStatus::AlreadyCreated) &&
                   !BaselineCommit.empty() && Marker.has_value();
        }
    };

    class GitWorktreeBaselineCreator final
    {
    public:
        explicit GitWorktreeBaselineCreator(
            std::filesystem::path gitExecutable,
            Microsoft::Terminal::TaskExecution::GitCommandExecutor executor = {});

        [[nodiscard]] GitWorktreeBaselineResult Create(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker,
            const GitWorktreeSourceJournal& journal,
            HANDLE cancellationEvent,
            uint32_t timeoutMilliseconds =
                DefaultGitWorktreeBaselineTimeoutMilliseconds) const noexcept;

    private:
        std::filesystem::path _gitExecutable;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor _executor;
    };
}
