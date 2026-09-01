// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreePreflightInternal.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        GitWorktreePreflightResult _failure(
            const GitWorktreePreflightStatus status,
            std::string error)
        {
            GitWorktreePreflightResult result;
            result.Status = status;
            result.SystemError = status == GitWorktreePreflightStatus::InvalidGitOutput ?
                                     ERROR_INVALID_DATA :
                                     ERROR_ALREADY_EXISTS;
            result.Error = std::move(error);
            return result;
        }

        std::optional<std::filesystem::path> _widePath(
            const std::string_view value)
        {
            if (value.empty() ||
                value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return std::nullopt;
            }
            const auto required = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0);
            if (required <= 0)
            {
                return std::nullopt;
            }
            std::wstring decoded(static_cast<size_t>(required), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    decoded.data(),
                    required) != required)
            {
                return std::nullopt;
            }
            const std::filesystem::path path{ decoded };
            return path.is_absolute() && path.has_root_path() ?
                       std::optional<std::filesystem::path>{ path.lexically_normal() } :
                       std::nullopt;
        }

        std::optional<bool> _samePath(
            const std::string_view left,
            const std::string_view right)
        {
            const auto leftPath = _widePath(left);
            const auto rightPath = _widePath(right);
            if (!leftPath || !rightPath ||
                leftPath->native().size() > static_cast<size_t>(INT_MAX) ||
                rightPath->native().size() > static_cast<size_t>(INT_MAX))
            {
                return std::nullopt;
            }
            return CompareStringOrdinal(
                       leftPath->native().data(),
                       static_cast<int>(leftPath->native().size()),
                       rightPath->native().data(),
                       static_cast<int>(rightPath->native().size()),
                       TRUE) == CSTR_EQUAL;
        }

        GitWorktreePreflightResult _success(
            const GitWorktreePreflightDisposition disposition,
            const GitWorktreeRegistration* registration = nullptr)
        {
            GitWorktreePreflightResult result;
            result.Status = GitWorktreePreflightStatus::Succeeded;
            result.Disposition = disposition;
            if (registration)
            {
                result.ExistingRegistration = *registration;
            }
            return result;
        }
    }

    GitWorktreePreflightResult EvaluateGitWorktreeCollision(
        const GitWorktreeLayout& layout,
        const GitWorktreeSourceJournal& journal,
        const std::span<const GitWorktreeRegistration> registrations,
        const std::optional<std::string>& branchHead,
        const GitManagedWorktreePathState pathState,
        const GitWorktreePreflightMode mode) noexcept
    {
        try
        {
            const GitWorktreeRegistration* pathRegistration{};
            const GitWorktreeRegistration* branchRegistration{};
            for (const auto& registration : registrations)
            {
                const auto matchesPath = _samePath(
                    registration.Path,
                    layout.WorktreePathUtf8);
                if (!matchesPath)
                {
                    return _failure(
                        GitWorktreePreflightStatus::InvalidGitOutput,
                        "Git Worktree registration path is invalid UTF-8 or not absolute");
                }
                if (*matchesPath)
                {
                    if (pathRegistration)
                    {
                        return _failure(
                            GitWorktreePreflightStatus::InvalidGitOutput,
                            "Git Worktree list repeats the managed path");
                    }
                    pathRegistration = &registration;
                }
                if (registration.BranchRef == layout.BranchRef)
                {
                    if (branchRegistration)
                    {
                        return _failure(
                            GitWorktreePreflightStatus::InvalidGitOutput,
                            "Git Worktree list repeats the managed branch");
                    }
                    branchRegistration = &registration;
                }
            }

            if (branchRegistration && branchRegistration != pathRegistration)
            {
                return _failure(
                    GitWorktreePreflightStatus::BranchCollision,
                    "The deterministic Task branch is checked out elsewhere");
            }
            if (!pathRegistration)
            {
                if (branchHead)
                {
                    return _failure(
                        GitWorktreePreflightStatus::BranchCollision,
                        "The deterministic Task branch already exists without owned Worktree proof");
                }
                if (pathState == GitManagedWorktreePathState::SafeDirectory)
                {
                    return _failure(
                        GitWorktreePreflightStatus::PathCollision,
                        "The deterministic Task Worktree path is not registered to this Task");
                }
                return _success(GitWorktreePreflightDisposition::CreateNew);
            }

            if (mode == GitWorktreePreflightMode::RequireUnoccupied)
            {
                return _failure(
                    pathRegistration->BranchRef == layout.BranchRef || branchHead ?
                        GitWorktreePreflightStatus::BranchCollision :
                        GitWorktreePreflightStatus::PathCollision,
                    "A deterministic Task Worktree resource already exists");
            }
            if (pathState != GitManagedWorktreePathState::SafeDirectory ||
                pathRegistration->Bare || pathRegistration->Locked ||
                pathRegistration->Prunable ||
                pathRegistration->Head != journal.SourceHead)
            {
                return _failure(
                    GitWorktreePreflightStatus::PathCollision,
                    "Existing Task Worktree state does not match its recovery proof");
            }
            if (pathRegistration->Detached)
            {
                if (branchHead)
                {
                    return _failure(
                        GitWorktreePreflightStatus::BranchCollision,
                        "Detached Task Worktree recovery found an existing Task branch");
                }
                return _success(
                    GitWorktreePreflightDisposition::ResumeDetached,
                    pathRegistration);
            }
            if (pathRegistration->BranchRef != layout.BranchRef ||
                !branchHead || *branchHead != pathRegistration->Head)
            {
                return _failure(
                    pathRegistration->BranchRef == layout.BranchRef ?
                        GitWorktreePreflightStatus::BranchCollision :
                        GitWorktreePreflightStatus::PathCollision,
                    "Existing Task Worktree branch does not match its recovery proof");
            }
            return _success(
                GitWorktreePreflightDisposition::ResumeBranch,
                pathRegistration);
        }
        catch (...)
        {
            GitWorktreePreflightResult result;
            result.Status = GitWorktreePreflightStatus::ResourceFailure;
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error = "Git Worktree collision evaluation exhausted local resources";
            return result;
        }
    }
}
