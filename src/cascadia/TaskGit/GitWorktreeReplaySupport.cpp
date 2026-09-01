// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeReplayInternal.h"

namespace Microsoft::Terminal::TaskGit::details
{
    GitWorktreeReplayResult GitWorktreeReplayFailure(
        const GitWorktreeReplayStatus status,
        const DWORD systemError,
        std::string error)
    {
        GitWorktreeReplayResult result;
        result.Status = status;
        result.SystemError = systemError;
        result.Error = std::move(error);
        return result;
    }

    GitWorktreeReplayResult PreserveGitWorktreeReplayFailure(
        GitWorktreeReplayResult result,
        const GitWorktreeLayout& layout,
        const bool trackedApplied,
        const size_t copied,
        const size_t reused)
    {
        result.TrackedApplied = trackedApplied;
        result.FilesCopied = copied;
        result.FilesReused = reused;
        result.Preserved = true;
        result.PreservedPath = layout.WorktreePathUtf8;
        return result;
    }

    GitWorktreeReplayStatus MapGitWorktreeReplayInspectionStatus(
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus;
        switch (status)
        {
        case GitRepositoryInspectionStatus::Cancelled:
            return GitWorktreeReplayStatus::Cancelled;
        case GitRepositoryInspectionStatus::TimedOut:
            return GitWorktreeReplayStatus::TimedOut;
        case GitRepositoryInspectionStatus::LockFailed:
            return GitWorktreeReplayStatus::LockFailed;
        case GitRepositoryInspectionStatus::ResourceFailure:
            return GitWorktreeReplayStatus::ResourceFailure;
        default:
            return GitWorktreeReplayStatus::InspectionFailed;
        }
    }

    GitWorktreeReplayStatus MapGitWorktreeReplayLockStatus(
        const Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus;
        switch (status)
        {
        case TaskDirectoryLockStatus::Cancelled:
            return GitWorktreeReplayStatus::Cancelled;
        case TaskDirectoryLockStatus::TimedOut:
            return GitWorktreeReplayStatus::TimedOut;
        case TaskDirectoryLockStatus::ResourceFailure:
            return GitWorktreeReplayStatus::ResourceFailure;
        default:
            return GitWorktreeReplayStatus::LockFailed;
        }
    }

    GitWorktreeReplayStatus MapGitWorktreeReplayCommandStatus(
        const Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitCommandStatus;
        switch (status)
        {
        case GitCommandStatus::Cancelled:
            return GitWorktreeReplayStatus::Cancelled;
        case GitCommandStatus::TimedOut:
            return GitWorktreeReplayStatus::TimedOut;
        case GitCommandStatus::InvalidRequest:
            return GitWorktreeReplayStatus::InvalidRequest;
        case GitCommandStatus::ResourceFailure:
            return GitWorktreeReplayStatus::ResourceFailure;
        default:
            return GitWorktreeReplayStatus::GitFailure;
        }
    }

    GitWorktreeReplayStatus MapGitWorktreeReplayProofStatus(
        const GitWorktreeReplayProofStatus status) noexcept
    {
        switch (status)
        {
        case GitWorktreeReplayProofStatus::Changed:
            return GitWorktreeReplayStatus::TargetChanged;
        case GitWorktreeReplayProofStatus::ResourceFailure:
            return GitWorktreeReplayStatus::ResourceFailure;
        case GitWorktreeReplayProofStatus::InvalidOutput:
            return GitWorktreeReplayStatus::VerificationFailed;
        default:
            return GitWorktreeReplayStatus::GitFailure;
        }
    }

    GitWorktreePreflightStatus MapGitWorktreeReplayPreflightCommandStatus(
        const GitWorktreeReplayStatus status) noexcept
    {
        switch (status)
        {
        case GitWorktreeReplayStatus::Cancelled:
            return GitWorktreePreflightStatus::Cancelled;
        case GitWorktreeReplayStatus::TimedOut:
            return GitWorktreePreflightStatus::TimedOut;
        case GitWorktreeReplayStatus::ResourceFailure:
            return GitWorktreePreflightStatus::ResourceFailure;
        default:
            return GitWorktreePreflightStatus::GitFailure;
        }
    }

    std::optional<std::filesystem::path> DecodeGitWorktreeReplayPath(
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

    GitWorktreeReplayResult GitWorktreeReplayCopyFailure(
        const GitWorktreeFileCopyResult& copy,
        const GitWorktreeLayout& layout,
        const std::string& path,
        const bool trackedApplied,
        const size_t copied,
        const size_t reused)
    {
        auto result = GitWorktreeReplayFailure(
            copy.Status == GitWorktreeFileCopyStatus::Cancelled ?
                GitWorktreeReplayStatus::Cancelled :
            copy.Status == GitWorktreeFileCopyStatus::ResourceFailure ?
                GitWorktreeReplayStatus::ResourceFailure :
                GitWorktreeReplayStatus::CopyFailed,
            copy.SystemError,
            copy.Error.empty() ?
                "Task Worktree untracked-file replay failed" :
                copy.Error);
        result.FileCopyStatus = copy.Status;
        result.FailurePath = path;
        return PreserveGitWorktreeReplayFailure(
            std::move(result),
            layout,
            trackedApplied,
            copied,
            reused);
    }

    GitWorktreeReplayResult GitWorktreeReplayProofFailure(
        const GitWorktreeReplayProofResult& proof,
        const GitWorktreeReplayResult& commandFailure)
    {
        if (commandFailure.Status != GitWorktreeReplayStatus::GitFailure ||
            !commandFailure.Error.empty())
        {
            return commandFailure;
        }
        return GitWorktreeReplayFailure(
            MapGitWorktreeReplayProofStatus(proof.Status),
            proof.SystemError,
            proof.Error);
    }
}
