// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitSourceSnapshot.h"
#include "GitWorktreeMarker.h"

#include "GitCommandTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskGit
{
    inline constexpr uint32_t GitWorktreeSourceJournalFormatVersion{ 1 };
    inline constexpr size_t MaximumGitWorktreeSourceJournalBytes{
        Microsoft::Terminal::TaskExecution::MaximumGitOutputBytes + 1024 * 1024
    };

    struct GitWorktreeSourceJournal
    {
        uint32_t FormatVersion{ GitWorktreeSourceJournalFormatVersion };
        Microsoft::Terminal::TaskCore::TaskId Task;
        std::string OwnershipNonce;
        std::string RepositoryIdentityDigest;
        std::string SourceWorktreePath;
        Microsoft::Terminal::TaskExecution::GitObjectFormat ObjectFormat{
            Microsoft::Terminal::TaskExecution::GitObjectFormat::Sha1
        };
        std::string SourceHead;
        std::string SourceReference;
        std::string TrackedSnapshotCommit;
        std::string WorktreeTree;
        std::string IndexTree;
        std::string StashReference;
        std::string StatusDigest;
        std::string SnapshotDigest;
        std::vector<GitSourceFileRecord> UntrackedFiles;
        uint64_t UntrackedBytes{};
    };

    enum class GitWorktreeSourceJournalStatus
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

    struct GitWorktreeSourceJournalResult
    {
        GitWorktreeSourceJournalStatus Status{
            GitWorktreeSourceJournalStatus::IoFailure
        };
        std::optional<GitWorktreeSourceJournal> Journal;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitWorktreeSourceJournalStatus::Succeeded &&
                   Journal.has_value();
        }
    };

    [[nodiscard]] GitWorktreeSourceJournalResult MakeGitWorktreeSourceJournal(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitSourceSnapshot& snapshot) noexcept;
    [[nodiscard]] bool ValidateGitWorktreeSourceJournal(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal) noexcept;
    [[nodiscard]] GitWorktreeSourceJournalResult CreateGitWorktreeSourceJournal(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal) noexcept;
    [[nodiscard]] GitWorktreeSourceJournalResult ReadGitWorktreeSourceJournal(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker) noexcept;
}

namespace Microsoft::Terminal::TaskGit::details
{
    [[nodiscard]] bool ValidateGitWorktreeSourceJournalShape(
        const GitWorktreeSourceJournal& journal) noexcept;
    [[nodiscard]] std::optional<std::vector<uint8_t>>
    EncodeGitWorktreeSourceJournal(
        const GitWorktreeSourceJournal& journal) noexcept;
    [[nodiscard]] GitWorktreeSourceJournalResult DecodeGitWorktreeSourceJournal(
        std::span<const uint8_t> bytes) noexcept;
}
