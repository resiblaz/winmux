// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeGitWorktreeSetup.h"

#include "GitWorktreeSourceJournal.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    struct RuntimeGitWorktreeLayoutEvidenceResult
    {
        bool Succeeded{};
        std::optional<Microsoft::Terminal::TaskGit::GitWorktreeLayout> Layout;
        std::optional<std::filesystem::path> SourcePath;
        std::optional<Microsoft::Terminal::TaskGit::GitWorktreeMarker>
            ExpectedMarker;
        DWORD SystemError{};
        std::string Error;
    };

    struct RuntimeGitWorktreeEvidenceResult
    {
        bool Succeeded{};
        std::optional<Microsoft::Terminal::TaskGit::GitWorktreeMarker> Marker;
        std::optional<Microsoft::Terminal::TaskGit::GitWorktreeSourceJournal>
            Journal;
        DWORD SystemError{};
        std::string Error;
    };

    [[nodiscard]] RuntimeGitWorktreeLayoutEvidenceResult
    PrepareRuntimeGitWorktreeLayoutEvidence(
        const Microsoft::Terminal::TaskCore::TaskGitWorktreeRecord& record,
        const std::filesystem::path& runtimeDataRoot) noexcept;
    [[nodiscard]] RuntimeGitWorktreeEvidenceResult LoadRuntimeGitWorktreeEvidence(
        const Microsoft::Terminal::TaskCore::TaskGitWorktreeRecord& record,
        const Microsoft::Terminal::TaskGit::GitWorktreeLayout& layout,
        const Microsoft::Terminal::TaskGit::GitWorktreeMarker& expectedMarker) noexcept;
}
