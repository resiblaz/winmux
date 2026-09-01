// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeLayout.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskCore;
        using namespace Microsoft::Terminal::TaskExecution;

        GitWorktreeLayoutResult _failure(
            const GitWorktreeLayoutStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreeLayoutResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        std::filesystem::path _extendedPath(const std::filesystem::path& path)
        {
            const auto& value = path.native();
            return value.starts_with(L"\\\\") ?
                       std::filesystem::path{ LR"(\\?\UNC\)" + value.substr(2) } :
                       std::filesystem::path{ LR"(\\?\)" + value };
        }

        std::optional<std::string> _strictUtf8(
            const std::wstring_view value)
        {
            if (value.empty() ||
                value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return std::nullopt;
            }
            const auto required = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (required <= 0)
            {
                return std::nullopt;
            }
            std::string result(static_cast<size_t>(required), '\0');
            return WideCharToMultiByte(
                       CP_UTF8,
                       WC_ERR_INVALID_CHARS,
                       value.data(),
                       static_cast<int>(value.size()),
                       result.data(),
                       required,
                       nullptr,
                       nullptr) == required ?
                       std::optional<std::string>{ std::move(result) } :
                       std::nullopt;
        }

        DirectoryIdentityResult _ensureManagedChild(
            const CanonicalDirectoryIdentity& parent,
            const std::wstring_view name)
        {
            const auto path = parent.CanonicalPath() / std::wstring{ name };
            const auto extended = _extendedPath(path);
            if (CreateDirectoryW(extended.c_str(), nullptr) == FALSE)
            {
                const auto error = GetLastError();
                if (error != ERROR_ALREADY_EXISTS)
                {
                    DirectoryIdentityResult result;
                    result.Status = DirectoryIdentityStatus::OpenFailed;
                    result.SystemError = error;
                    result.Error = "Managed Git Worktree directory could not be created";
                    return result;
                }
            }
            return ResolveCanonicalDirectoryIdentity(
                path,
                DirectoryIdentityPolicy::Worktree);
        }
    }

    GitWorktreeLayoutResult PrepareGitWorktreeLayout(
        const std::filesystem::path& runtimeDataRoot,
        const Microsoft::Terminal::TaskCore::TaskId& task) noexcept
    {
        try
        {
            if (runtimeDataRoot.empty() ||
                !Microsoft::Terminal::TaskCore::IsCanonicalUuid(task.Value()))
            {
                return _failure(
                    GitWorktreeLayoutStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Managed Git Worktree layout request is invalid");
            }
            const auto runtimeRoot = ResolveCanonicalDirectoryIdentity(
                runtimeDataRoot,
                DirectoryIdentityPolicy::Worktree);
            if (!runtimeRoot.Succeeded())
            {
                return _failure(
                    GitWorktreeLayoutStatus::RuntimeRootRejected,
                    runtimeRoot.SystemError,
                    "Runtime data root is unsafe for managed Git Worktrees");
            }
            const auto taskRoot = _ensureManagedChild(
                *runtimeRoot.Identity,
                GitWorktreeRootDirectoryName);
            const auto versionRoot = taskRoot.Succeeded() ?
                                         _ensureManagedChild(
                                             *taskRoot.Identity,
                                             GitWorktreeLayoutVersionDirectoryName) :
                                         DirectoryIdentityResult{};
            const auto stateRoot = versionRoot.Succeeded() ?
                                       _ensureManagedChild(
                                           *versionRoot.Identity,
                                           GitWorktreeStateDirectoryName) :
                                       DirectoryIdentityResult{};
            if (!taskRoot.Succeeded() || !versionRoot.Succeeded() ||
                !stateRoot.Succeeded())
            {
                const auto& failed = !taskRoot.Succeeded() ?
                                         taskRoot :
                                     !versionRoot.Succeeded() ?
                                         versionRoot :
                                         stateRoot;
                return _failure(
                    GitWorktreeLayoutStatus::ManagedRootFailure,
                    failed.SystemError,
                    "Managed Git Worktree root could not be prepared safely");
            }

            const auto taskName = std::wstring{
                task.Value().begin(),
                task.Value().end()
            };
            const auto worktreePath =
                versionRoot.Identity->CanonicalPath() / taskName;
            const auto markerPath = stateRoot.Identity->CanonicalPath() /
                                    (taskName + L".owner-v1.bin");
            const auto snapshotPath = stateRoot.Identity->CanonicalPath() /
                                      (taskName + L".source-v1.bin");
            const auto copyStagingPath = stateRoot.Identity->CanonicalPath() /
                                         (taskName + L".copy-v1.tmp");
            if (worktreePath.native().size() >
                    Microsoft::Terminal::TaskExecution::MaximumTaskExecutionPathCharacters ||
                markerPath.native().size() >
                    Microsoft::Terminal::TaskExecution::MaximumTaskExecutionPathCharacters ||
                snapshotPath.native().size() >
                    Microsoft::Terminal::TaskExecution::MaximumTaskExecutionPathCharacters ||
                copyStagingPath.native().size() >
                    Microsoft::Terminal::TaskExecution::MaximumTaskExecutionPathCharacters)
            {
                return _failure(
                    GitWorktreeLayoutStatus::InvalidPath,
                    ERROR_FILENAME_EXCED_RANGE,
                    "Managed Git Worktree layout exceeds the path bound");
            }
            const auto worktreeUtf8 = _strictUtf8(worktreePath.native());
            if (!worktreeUtf8)
            {
                return _failure(
                    GitWorktreeLayoutStatus::InvalidPath,
                    ERROR_NO_UNICODE_TRANSLATION,
                    "Managed Git Worktree path is not valid UTF-8");
            }

            GitWorktreeLayoutResult result;
            result.Status = GitWorktreeLayoutStatus::Succeeded;
            result.Layout.emplace();
            result.Layout->Task = task;
            result.Layout->RuntimeRoot = *runtimeRoot.Identity;
            result.Layout->ManagedRoot = *versionRoot.Identity;
            result.Layout->StateRoot = *stateRoot.Identity;
            result.Layout->WorktreePath = worktreePath;
            result.Layout->MarkerPath = markerPath;
            result.Layout->SnapshotPath = snapshotPath;
            result.Layout->CopyStagingPath = copyStagingPath;
            result.Layout->WorktreePathUtf8 = *worktreeUtf8;
            result.Layout->BranchRef = "refs/heads/wt-task/" + task.Value();
            result.Layout->BranchName = "wt-task/" + task.Value();
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeLayoutStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Managed Git Worktree layout exhausted local resources");
        }
    }
}
