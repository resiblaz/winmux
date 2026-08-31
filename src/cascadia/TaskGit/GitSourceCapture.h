// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitSnapshotFile.h"
#include "GitSourceSnapshot.h"

#include "GitRepositoryInspector.h"

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr uint32_t DefaultGitSourceCaptureTimeoutMilliseconds{
        120000
    };
    inline constexpr uint32_t MaximumGitSourceCaptureTimeoutMilliseconds{
        300000
    };

    class GitSourceCapture final
    {
    public:
        explicit GitSourceCapture(
            std::filesystem::path gitExecutable,
            Microsoft::Terminal::TaskExecution::GitCommandExecutor executor = {},
            GitSnapshotFileHasher fileHasher = {});

        [[nodiscard]] GitSourceCaptureResult Capture(
            const std::filesystem::path& sourcePath,
            HANDLE cancellationEvent,
            uint32_t timeoutMilliseconds =
                DefaultGitSourceCaptureTimeoutMilliseconds) const noexcept;

    private:
        std::filesystem::path _gitExecutable;
        Microsoft::Terminal::TaskExecution::GitCommandExecutor _executor;
        GitSnapshotFileHasher _fileHasher;
    };
}
