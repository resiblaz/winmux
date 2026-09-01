// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitSourceSnapshot.h"

#include "DirectoryIdentity.h"

namespace Microsoft::Terminal::TaskGit
{
    enum class GitWorktreeFileCopyStatus
    {
        Succeeded,
        AlreadyPresent,
        InvalidRequest,
        Cancelled,
        SourceChanged,
        DestinationCollision,
        UnsafeSource,
        UnsafeDestination,
        IoFailure,
        ResourceFailure,
    };

    struct GitWorktreeFileCopyResult
    {
        GitWorktreeFileCopyStatus Status{
            GitWorktreeFileCopyStatus::IoFailure
        };
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeFileCopyStatus::Succeeded ||
                   Status == GitWorktreeFileCopyStatus::AlreadyPresent;
        }

        [[nodiscard]] bool Copied() const noexcept
        {
            return Status == GitWorktreeFileCopyStatus::Succeeded;
        }
    };

    [[nodiscard]] GitWorktreeFileCopyResult CopyGitWorktreeSnapshotFile(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            sourceRoot,
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            destinationRoot,
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            stateRoot,
        const std::filesystem::path& stagingPath,
        const GitSourceFileRecord& file,
        HANDLE cancellationEvent) noexcept;
}
