// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitSourceDigest.h"

#include "TaskExecutionCrypto.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        std::optional<std::string> _digest(const std::span<const uint8_t> payload)
        {
            const auto digest =
                Microsoft::Terminal::TaskExecution::details::TaskExecutionSha256(
                    payload);
            return digest ?
                       std::optional<std::string>{
                           Microsoft::Terminal::TaskExecution::details::LowerHex(
                               *digest) } :
                       std::nullopt;
        }

        void _appendUint64(std::vector<uint8_t>& payload, const uint64_t value)
        {
            for (size_t index = 0; index < sizeof(value); ++index)
            {
                payload.push_back(static_cast<uint8_t>(value >> (index * 8)));
            }
        }

        void _appendText(
            std::vector<uint8_t>& payload,
            const std::string_view value)
        {
            _appendUint64(payload, value.size());
            payload.insert(payload.end(), value.begin(), value.end());
        }
    }

    std::optional<std::string> ComputeGitSourceStatusDigest(
        const std::string_view statusOutput)
    {
        return _digest(std::span<const uint8_t>{
            reinterpret_cast<const uint8_t*>(statusOutput.data()),
            statusOutput.size(),
        });
    }

    std::optional<std::string> ComputeGitSourceSnapshotDigest(
        const GitSourceSnapshot& snapshot)
    {
        using Microsoft::Terminal::TaskExecution::GitObjectFormat;
        constexpr std::string_view domain{ "wt-task-git-source-snapshot-v1" };
        std::vector<uint8_t> payload;
        payload.insert(payload.end(), domain.begin(), domain.end());
        payload.push_back(0);
        _appendText(payload, snapshot.Repository.CommonDirectoryIdentity.Digest());
        _appendText(payload, snapshot.Repository.WorktreeIdentity.Digest());
        _appendText(payload, snapshot.Repository.Head);
        _appendText(payload, snapshot.Repository.BranchRef);
        _appendUint64(
            payload,
            snapshot.Repository.ObjectFormat == GitObjectFormat::Sha1 ? 1 : 2);
        _appendUint64(payload, snapshot.Repository.LinkedWorktree ? 1 : 0);
        _appendUint64(payload, snapshot.Repository.DetachedHead ? 1 : 0);
        // stash create embeds wall-clock identity in its transient commit.
        // The trees are the canonical tracked-state proof; including the
        // commit object would make equal source states hash differently.
        _appendText(payload, snapshot.WorktreeTree);
        _appendText(payload, snapshot.IndexTree);
        _appendText(payload, snapshot.StashReference);
        _appendText(payload, snapshot.StatusDigest);
        _appendUint64(payload, snapshot.Status.HasStagedChanges ? 1 : 0);
        _appendUint64(payload, snapshot.Status.HasUnstagedChanges ? 1 : 0);
        _appendUint64(payload, snapshot.Status.IgnoredFiles);
        _appendUint64(payload, snapshot.UntrackedBytes);
        _appendUint64(payload, snapshot.UntrackedFiles.size());
        for (const auto& file : snapshot.UntrackedFiles)
        {
            _appendText(payload, file.RelativePath);
            _appendUint64(payload, static_cast<uint8_t>(file.Type));
            _appendUint64(payload, file.Size);
            _appendText(payload, file.Sha256);
        }
        return _digest(payload);
    }
}
