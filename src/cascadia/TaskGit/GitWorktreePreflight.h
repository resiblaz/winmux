// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeSourceJournal.h"

#include "GitRepositoryInspector.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr uint32_t DefaultGitWorktreePreflightTimeoutMilliseconds{
        120000
    };
    inline constexpr uint32_t MaximumGitWorktreePreflightTimeoutMilliseconds{
        300000
    };

    enum class GitWorktreePreflightMode
    {
        RequireUnoccupied,
        // Use only after TaskCore identity and both ownership sidecars have
        // been loaded and matched. This mode never adopts branch-only or
        // unregistered-directory remnants.
        AllowOwnedRecovery,
    };

    enum class GitWorktreePreflightDisposition
    {
        None,
        CreateNew,
        ResumeDetached,
        ResumeBranch,
    };

    enum class GitWorktreePreflightStatus
    {
        Succeeded,
        InvalidRequest,
        InspectionFailed,
        LockFailed,
        Cancelled,
        TimedOut,
        RepositoryChanged,
        UnsupportedSparseCheckout,
        UnsupportedSubmodules,
        UnsupportedFilter,
        NetworkMaterialRequired,
        MissingObject,
        BranchCollision,
        PathCollision,
        UnsafePath,
        InvalidGitOutput,
        GitFailure,
        ResourceFailure,
    };

    struct GitWorktreeRegistration
    {
        std::string Path;
        std::string Head;
        std::string BranchRef;
        bool Detached{};
        bool Bare{};
        bool Locked{};
        bool Prunable{};
    };

    struct GitWorktreePreflightResult
    {
        GitWorktreePreflightStatus Status{
            GitWorktreePreflightStatus::GitFailure
        };
        GitWorktreePreflightDisposition Disposition{
            GitWorktreePreflightDisposition::None
        };
        std::optional<GitWorktreeRegistration> ExistingRegistration;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreePreflightStatus::Succeeded &&
                   Disposition != GitWorktreePreflightDisposition::None;
        }
    };

    class GitWorktreePreflightInspector final
    {
    public:
        explicit GitWorktreePreflightInspector(
            std::filesystem::path gitExecutable,
            Microsoft::Terminal::TaskExecution::GitCommandExecutor executor = {});

        [[nodiscard]] GitWorktreePreflightResult Inspect(
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker,
            const GitWorktreeSourceJournal& journal,
            HANDLE cancellationEvent,
            GitWorktreePreflightMode mode,
            uint32_t timeoutMilliseconds =
                DefaultGitWorktreePreflightTimeoutMilliseconds) const noexcept;

    private:
        std::filesystem::path _gitExecutable;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor _executor;
    };
}

namespace Microsoft::Terminal::TaskGit::details
{
    struct GitWorktreeListParseResult
    {
        bool Succeeded{};
        std::vector<GitWorktreeRegistration> Registrations;
        std::string Error;
    };

    [[nodiscard]] GitWorktreeListParseResult ParseGitWorktreeList(
        std::string_view output,
        Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat) noexcept;
    [[nodiscard]] std::optional<bool> GitIndexContainsSubmodules(
        std::string_view output,
        Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat) noexcept;
    [[nodiscard]] std::optional<bool> GitTreeContainsSubmodules(
        std::string_view output,
        Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat) noexcept;
    [[nodiscard]] std::optional<std::vector<std::string>>
    ParseGitFilterDriverNames(std::string_view output) noexcept;
    [[nodiscard]] std::optional<bool> GitConfigContainsTrueValue(
        std::string_view output) noexcept;
}
