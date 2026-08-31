// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "DirectoryIdentity.h"
#include "GitSourceSnapshot.h"

#include <functional>
#include <optional>
#include <string_view>

namespace Microsoft::Terminal::TaskGit
{
    enum class GitSnapshotFileStatus
    {
        Succeeded,
        InvalidPath,
        Missing,
        UnsafeAttributes,
        IdentityMismatch,
        TooLarge,
        ReadFailure,
        SourceChanged,
        Cancelled,
        ResourceFailure,
    };

    struct GitSnapshotFileResult
    {
        GitSnapshotFileStatus Status{ GitSnapshotFileStatus::ReadFailure };
        std::optional<GitSourceFileRecord> File;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitSnapshotFileStatus::Succeeded && File.has_value();
        }
    };

    using GitSnapshotFileHasher = std::function<GitSnapshotFileResult(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&,
        std::string_view,
        HANDLE)>;

    [[nodiscard]] GitSnapshotFileResult HashGitSnapshotFile(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity& root,
        std::string_view relativePath,
        HANDLE cancellationEvent) noexcept;
}

namespace Microsoft::Terminal::TaskGit::details
{
    [[nodiscard]] GitSnapshotFileStatus ClassifyGitSnapshotFileAttributes(
        DWORD attributes) noexcept;
}
