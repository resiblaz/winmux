// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskGit::details
{
    enum class GitWorktreeMetadataFileStatus
    {
        Succeeded,
        Missing,
        Collision,
        UnsafePath,
        IoFailure,
        ResourceFailure,
    };

    struct GitWorktreeMetadataFileResult
    {
        GitWorktreeMetadataFileStatus Status{
            GitWorktreeMetadataFileStatus::IoFailure
        };
        std::vector<uint8_t> Bytes;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeMetadataFileStatus::Succeeded;
        }
    };

    [[nodiscard]] GitWorktreeMetadataFileResult ReadGitWorktreeMetadataFile(
        const std::filesystem::path& path,
        size_t maximumBytes) noexcept;
    [[nodiscard]] GitWorktreeMetadataFileResult WriteNewGitWorktreeMetadataFile(
        const std::filesystem::path& path,
        std::span<const uint8_t> bytes) noexcept;
    [[nodiscard]] GitWorktreeMetadataFileResult ReplaceGitWorktreeMetadataFile(
        const std::filesystem::path& stagingPath,
        const std::filesystem::path& destinationPath) noexcept;
    void DeleteGitWorktreeMetadataFile(
        const std::filesystem::path& path) noexcept;
}
