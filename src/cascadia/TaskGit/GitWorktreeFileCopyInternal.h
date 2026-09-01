// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitFileHash.h"
#include "GitWorktreeFileCopy.h"

namespace Microsoft::Terminal::TaskGit::details
{
    class GitWorktreeCopyHandle final
    {
    public:
        GitWorktreeCopyHandle() noexcept = default;
        ~GitWorktreeCopyHandle();
        GitWorktreeCopyHandle(const GitWorktreeCopyHandle&) = delete;
        GitWorktreeCopyHandle& operator=(const GitWorktreeCopyHandle&) = delete;
        GitWorktreeCopyHandle(GitWorktreeCopyHandle&& other) noexcept;
        GitWorktreeCopyHandle& operator=(GitWorktreeCopyHandle&& other) noexcept;

        void Reset(HANDLE value = nullptr) noexcept;
        HANDLE Value{};
    };

    struct GitWorktreeFileEvidence
    {
        FILE_STANDARD_INFO Standard{};
        FILE_BASIC_INFO Basic{};
        FILE_ID_INFO Identity{};
    };

    struct GitWorktreeDestinationInspection
    {
        bool Missing{};
        GitWorktreeFileCopyResult Result;
    };

    [[nodiscard]] GitWorktreeFileCopyResult GitWorktreeFileCopyResultFor(
        GitWorktreeFileCopyStatus status,
        DWORD systemError,
        std::string error);
    [[nodiscard]] std::filesystem::path ExtendedGitWorktreeCopyPath(
        const std::filesystem::path& path);
    [[nodiscard]] bool SameGitWorktreeCopyPath(
        const std::filesystem::path& left,
        const std::filesystem::path& right) noexcept;
    [[nodiscard]] bool ReadGitWorktreeFileEvidence(
        HANDLE handle,
        const std::filesystem::path& expectedPath,
        GitWorktreeFileEvidence& evidence,
        DWORD& systemError);
    [[nodiscard]] bool SameGitWorktreeFileEvidence(
        const GitWorktreeFileEvidence& left,
        const GitWorktreeFileEvidence& right) noexcept;
    [[nodiscard]] bool ValidateGitWorktreeCopyRoot(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity& root) noexcept;
    [[nodiscard]] std::optional<std::filesystem::path>
    DecodeGitWorktreeCopyRelativePath(std::string_view value);
    [[nodiscard]] bool RewindGitWorktreeCopyFile(
        HANDLE file,
        DWORD& systemError) noexcept;
    [[nodiscard]] GitWorktreeFileCopyResult MapGitWorktreeCopyHashFailure(
        const GitFileHashResult& hash,
        GitWorktreeFileCopyStatus failureStatus,
        std::string error);
    [[nodiscard]] GitWorktreeFileCopyResult EnsureGitWorktreeCopyParents(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            destinationRoot,
        const std::filesystem::path& relativeParent);
    [[nodiscard]] GitWorktreeFileCopyResult CleanupGitWorktreeCopyStaging(
        const std::filesystem::path& stagingPath);
    [[nodiscard]] GitWorktreeDestinationInspection InspectGitWorktreeCopyDestination(
        const std::filesystem::path& destination,
        const GitSourceFileRecord& expected,
        HANDLE cancellationEvent);
    [[nodiscard]] GitWorktreeFileCopyResult CopyGitWorktreeFileBytes(
        HANDLE source,
        HANDLE destination,
        uint64_t expectedBytes,
        HANDLE cancellationEvent);
}
