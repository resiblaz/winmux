// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "DirectoryIdentity.h"
#include "TaskIdentifiers.h"

#include <filesystem>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr std::wstring_view GitWorktreeRootDirectoryName{
        L"task-worktrees"
    };
    inline constexpr std::wstring_view GitWorktreeLayoutVersionDirectoryName{
        L"v1"
    };
    inline constexpr std::wstring_view GitWorktreeStateDirectoryName{
        L".state"
    };

    struct GitWorktreeLayout
    {
        Microsoft::Terminal::TaskCore::TaskId Task;
        Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity RuntimeRoot;
        Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity ManagedRoot;
        Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity StateRoot;
        std::filesystem::path WorktreePath;
        std::filesystem::path MarkerPath;
        std::filesystem::path SnapshotPath;
        std::filesystem::path CopyStagingPath;
        std::string WorktreePathUtf8;
        std::string BranchRef;
        std::string BranchName;
    };

    enum class GitWorktreeLayoutStatus
    {
        Succeeded,
        InvalidRequest,
        RuntimeRootRejected,
        ManagedRootFailure,
        InvalidPath,
        ResourceFailure,
    };

    struct GitWorktreeLayoutResult
    {
        GitWorktreeLayoutStatus Status{
            GitWorktreeLayoutStatus::ManagedRootFailure
        };
        std::optional<GitWorktreeLayout> Layout;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeLayoutStatus::Succeeded &&
                   Layout.has_value();
        }
    };

    [[nodiscard]] GitWorktreeLayoutResult PrepareGitWorktreeLayout(
        const std::filesystem::path& runtimeDataRoot,
        const Microsoft::Terminal::TaskCore::TaskId& task) noexcept;
}
