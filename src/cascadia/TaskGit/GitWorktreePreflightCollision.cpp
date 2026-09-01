// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreePreflightInternal.h"

#include "GitSourceCommandOutput.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        GitWorktreePreflightResult _failure(
            const GitWorktreePreflightStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreePreflightResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _samePath(
            const std::filesystem::path& left,
            const std::filesystem::path& right) noexcept
        {
            try
            {
                return left.native().size() <= static_cast<size_t>(INT_MAX) &&
                       right.native().size() <= static_cast<size_t>(INT_MAX) &&
                       CompareStringOrdinal(
                           left.native().data(),
                           static_cast<int>(left.native().size()),
                           right.native().data(),
                           static_cast<int>(right.native().size()),
                           TRUE) == CSTR_EQUAL;
            }
            catch (...)
            {
                return false;
            }
        }

        std::filesystem::path _extendedPath(
            const std::filesystem::path& path)
        {
            const auto& value = path.native();
            return value.starts_with(L"\\\\") ?
                       std::filesystem::path{ LR"(\\?\UNC\)" + value.substr(2) } :
                       std::filesystem::path{ LR"(\\?\)" + value };
        }

        struct ManagedPathInspection
        {
            std::optional<GitManagedWorktreePathState> State;
            GitWorktreePreflightResult Failure;
        };

        ManagedPathInspection _inspectManagedPath(
            const GitWorktreeLayout& layout)
        {
            using namespace Microsoft::Terminal::TaskExecution;
            const auto currentRoot = ResolveCanonicalDirectoryIdentity(
                layout.ManagedRoot.CanonicalPath(),
                DirectoryIdentityPolicy::Worktree);
            if (!currentRoot.Succeeded() || !layout.ManagedRoot.Valid() ||
                currentRoot.Identity->Digest() != layout.ManagedRoot.Digest() ||
                !_samePath(
                    layout.WorktreePath.parent_path(),
                    layout.ManagedRoot.CanonicalPath()))
            {
                return {
                    std::nullopt,
                    _failure(
                        GitWorktreePreflightStatus::UnsafePath,
                        ERROR_ACCESS_DENIED,
                        "Managed Task Worktree root identity is unsafe"),
                };
            }

            const auto attributes = GetFileAttributesW(
                _extendedPath(layout.WorktreePath).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const auto error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                {
                    return { GitManagedWorktreePathState::Absent, {} };
                }
                return {
                    std::nullopt,
                    _failure(
                        GitWorktreePreflightStatus::UnsafePath,
                        error,
                        "Managed Task Worktree path could not be inspected"),
                };
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (attributes &
                 (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0)
            {
                return {
                    std::nullopt,
                    _failure(
                        GitWorktreePreflightStatus::UnsafePath,
                        ERROR_ACCESS_DENIED,
                        "Managed Task Worktree path is not a direct directory"),
                };
            }
            const auto identity = ResolveCanonicalDirectoryIdentity(
                layout.WorktreePath,
                DirectoryIdentityPolicy::Worktree);
            if (!identity.Succeeded() ||
                !_samePath(
                    identity.Identity->CanonicalPath(),
                    layout.WorktreePath))
            {
                return {
                    std::nullopt,
                    _failure(
                        GitWorktreePreflightStatus::UnsafePath,
                        identity.SystemError == ERROR_SUCCESS ?
                            ERROR_ACCESS_DENIED :
                            identity.SystemError,
                        "Managed Task Worktree path identity is unsafe"),
                };
            }
            return { GitManagedWorktreePathState::SafeDirectory, {} };
        }
    }

    GitWorktreePreflightResult InspectGitWorktreeCollision(
        const GitWorktreeLayout& layout,
        const GitWorktreeSourceJournal& journal,
        const GitWorktreePreflightMode mode,
        const GitWorktreePreflightCommandInvoker& invoke) noexcept
    {
        try
        {
            const auto worktrees = invoke({
                L"worktree",
                L"list",
                L"--porcelain",
                L"-z",
            });
            if (!worktrees)
            {
                return worktrees.Failure;
            }
            if (worktrees.Result->ExitCode != 0)
            {
                return _failure(
                    GitWorktreePreflightStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Git Worktree registration query failed");
            }
            const auto parsedWorktrees = ParseGitWorktreeList(
                worktrees.Result->Stdout,
                journal.ObjectFormat);
            if (!parsedWorktrees.Succeeded)
            {
                return _failure(
                    GitWorktreePreflightStatus::InvalidGitOutput,
                    ERROR_INVALID_DATA,
                    parsedWorktrees.Error);
            }

            const auto branch = invoke({
                L"rev-parse",
                L"--verify",
                L"--quiet",
                WidenGitSourceAscii(layout.BranchRef),
            });
            if (!branch)
            {
                return branch.Failure;
            }
            std::optional<std::string> branchHead;
            if (branch.Result->ExitCode == 0)
            {
                branchHead = ParseGitSourceLine(*branch.Result);
                if (!branchHead ||
                    !IsGitSourceObjectId(*branchHead, journal.ObjectFormat))
                {
                    return _failure(
                        GitWorktreePreflightStatus::InvalidGitOutput,
                        ERROR_INVALID_DATA,
                        "Git Task branch reference output is invalid");
                }
            }
            else if (branch.Result->ExitCode != 1 ||
                     !branch.Result->Stdout.empty())
            {
                return _failure(
                    GitWorktreePreflightStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Git Task branch reference query failed");
            }

            const auto path = _inspectManagedPath(layout);
            if (!path.State)
            {
                return path.Failure;
            }
            return EvaluateGitWorktreeCollision(
                layout,
                journal,
                parsedWorktrees.Registrations,
                branchHead,
                *path.State,
                mode);
        }
        catch (...)
        {
            return _failure(
                GitWorktreePreflightStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree collision inspection exhausted local resources");
        }
    }
}
