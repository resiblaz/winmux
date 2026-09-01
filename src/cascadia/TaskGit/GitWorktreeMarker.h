// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeLayout.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr uint32_t GitWorktreeMarkerFormatVersion{ 1 };
    inline constexpr size_t MaximumGitWorktreeMarkerBytes{ 64 * 1024 };

    struct GitWorktreeMarker
    {
        uint32_t FormatVersion{ GitWorktreeMarkerFormatVersion };
        Microsoft::Terminal::TaskCore::TaskId Task;
        std::string RepositoryIdentityDigest;
        std::string BranchRef;
        std::string SnapshotDigest;
        std::string BaselineCommit;
        std::string OwnershipNonce;
        std::string WorktreePath;
    };

    enum class GitWorktreeMarkerStatus
    {
        Succeeded,
        InvalidRequest,
        Missing,
        Collision,
        UnsafePath,
        Malformed,
        IdentityMismatch,
        IoFailure,
        ResourceFailure,
    };

    struct GitWorktreeMarkerResult
    {
        GitWorktreeMarkerStatus Status{ GitWorktreeMarkerStatus::IoFailure };
        std::optional<GitWorktreeMarker> Marker;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeMarkerStatus::Succeeded &&
                   Marker.has_value();
        }
    };

    [[nodiscard]] bool ValidateGitWorktreeMarker(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker) noexcept;
    [[nodiscard]] GitWorktreeMarkerResult CreateGitWorktreeMarker(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker) noexcept;
    [[nodiscard]] GitWorktreeMarkerResult ReadGitWorktreeMarker(
        const GitWorktreeLayout& layout) noexcept;
    [[nodiscard]] GitWorktreeMarkerResult ReplaceGitWorktreeMarker(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& expected,
        const GitWorktreeMarker& replacement) noexcept;
}

namespace Microsoft::Terminal::TaskGit::details
{
    [[nodiscard]] std::optional<std::vector<uint8_t>> EncodeGitWorktreeMarker(
        const GitWorktreeMarker& marker) noexcept;
    [[nodiscard]] GitWorktreeMarkerResult DecodeGitWorktreeMarker(
        std::span<const uint8_t> bytes) noexcept;
}
