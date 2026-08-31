// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "DirectoryIdentity.h"
#include "GitCommandTypes.h"

#include <functional>
#include <optional>

namespace Microsoft::Terminal::TaskExecution
{
    inline constexpr uint32_t MinimumSupportedGitMajorVersion{ 2 };
    inline constexpr uint32_t MinimumSupportedGitMinorVersion{ 38 };
    inline constexpr uint32_t DefaultGitInspectionTimeoutMilliseconds{ 30000 };

    enum class GitObjectFormat
    {
        Sha1,
        Sha256,
    };

    struct GitVersion
    {
        uint32_t Major{};
        uint32_t Minor{};
        uint32_t Patch{};
    };

    struct GitRepositoryInspection
    {
        std::filesystem::path GitExecutable;
        CanonicalDirectoryIdentity WorktreeIdentity;
        CanonicalDirectoryIdentity CommonDirectoryIdentity;
        CanonicalDirectoryIdentity GitDirectoryIdentity;
        GitVersion Version;
        GitObjectFormat ObjectFormat{ GitObjectFormat::Sha1 };
        std::string Head;
        std::string BranchRef;
        bool LinkedWorktree{};
        bool DetachedHead{};
        int64_t CapturedAtUnixMilliseconds{};
    };

    enum class GitRepositoryInspectionStatus
    {
        Succeeded,
        InvalidRequest,
        DirectoryRejected,
        GitUnavailable,
        UnsupportedGitVersion,
        NotRepository,
        BareRepository,
        UnbornRepository,
        LockFailed,
        Cancelled,
        TimedOut,
        InvalidOutput,
        InspectionFailed,
        ResourceFailure,
    };

    struct GitRepositoryInspectionResult
    {
        GitRepositoryInspectionStatus Status{
            GitRepositoryInspectionStatus::InspectionFailed
        };
        std::optional<GitRepositoryInspection> Inspection;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitRepositoryInspectionStatus::Succeeded &&
                   Inspection.has_value();
        }
    };

    using GitCommandExecutor = std::function<GitCommandResult(
        const GitCommandRequest&)>;

    class GitRepositoryInspector final
    {
    public:
        explicit GitRepositoryInspector(
            std::filesystem::path gitExecutable,
            GitCommandExecutor executor = {});

        [[nodiscard]] GitRepositoryInspectionResult Inspect(
            const std::filesystem::path& path,
            HANDLE cancellationEvent,
            uint32_t timeoutMilliseconds =
                DefaultGitInspectionTimeoutMilliseconds) const noexcept;

    private:
        std::filesystem::path _gitExecutable;
        GitCommandExecutor _executor;
    };
}
