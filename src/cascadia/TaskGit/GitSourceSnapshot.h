// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitRepositoryInspector.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr size_t MaximumGitSnapshotUntrackedFiles{ 2000 };
    inline constexpr uint64_t MaximumGitSnapshotUntrackedBytes{
        200ull * 1024 * 1024
    };
    inline constexpr size_t MaximumGitSnapshotPathBytes{ 32 * 1024 };

    enum class GitSourceFileType : uint8_t
    {
        RegularFile = 1,
    };

    struct GitSourceFileRecord
    {
        std::string RelativePath;
        GitSourceFileType Type{ GitSourceFileType::RegularFile };
        uint64_t Size{};
        std::string Sha256;
    };

    struct GitSourceStatusSummary
    {
        bool HasStagedChanges{};
        bool HasUnstagedChanges{};
        size_t UntrackedFiles{};
        size_t IgnoredFiles{};
    };

    struct GitSourceSnapshot
    {
        Microsoft::Terminal::TaskExecution::GitRepositoryInspection Repository;
        GitSourceStatusSummary Status;
        std::string TrackedSnapshotCommit;
        std::string WorktreeTree;
        std::string IndexTree;
        std::string StashReference;
        std::string StatusDigest;
        std::vector<GitSourceFileRecord> UntrackedFiles;
        uint64_t UntrackedBytes{};
        std::string SnapshotDigest;
        int64_t CapturedAtUnixMilliseconds{};
    };

    enum class GitSourceCaptureStatus
    {
        Succeeded,
        InvalidRequest,
        InspectionFailed,
        LockFailed,
        Cancelled,
        TimedOut,
        GitFailure,
        InvalidGitOutput,
        UnsafeUntrackedFile,
        SourceChanged,
        FileCountExceeded,
        ByteLimitExceeded,
        ResourceFailure,
    };

    struct GitSourceCaptureResult
    {
        GitSourceCaptureStatus Status{ GitSourceCaptureStatus::GitFailure };
        std::optional<GitSourceSnapshot> Snapshot;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitSourceCaptureStatus::Succeeded &&
                   Snapshot.has_value();
        }
    };
}
