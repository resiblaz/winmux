// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitWorktreeReplayProof.h"

namespace Microsoft::Terminal::TaskGit::details
{
    struct GitWorktreeTreeAddition
    {
        std::string Path;
        std::string Mode;
        std::string Object;
    };

    struct GitWorktreeTreeDiffParseResult
    {
        bool Succeeded{};
        std::vector<GitWorktreeTreeAddition> Additions;
        std::string Error;
    };

    [[nodiscard]] GitWorktreeTreeDiffParseResult ParseGitWorktreeTreeAdditions(
        std::string_view output,
        Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat) noexcept;
    [[nodiscard]] bool GitWorktreeBaselineCandidateMatches(
        const GitWorktreeReplayProof& proof,
        const GitWorktreeSourceJournal& journal,
        std::span<const GitWorktreeTreeAddition> additions) noexcept;
    [[nodiscard]] bool GitWorktreeBaselineAdditionsMatch(
        const GitWorktreeSourceJournal& journal,
        std::span<const GitWorktreeTreeAddition> additions) noexcept;
    [[nodiscard]] std::string GitWorktreeBaselineSubject(
        const GitWorktreeMarker& marker);
    [[nodiscard]] bool ValidateGitWorktreeBaselineParents(
        std::string_view output,
        std::string_view expectedCommit,
        std::string_view expectedParent,
        Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat) noexcept;
}
