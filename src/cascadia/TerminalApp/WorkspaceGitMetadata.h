// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include <winrt/base.h>

namespace winrt::TerminalApp::implementation
{
    enum class WorkspaceGitCommand
    {
        ShowTopLevel,
        ShowCommonGitDirectory,
        ShowGitDirectory,
        VerifyHead,
        SymbolicHead,
        IsAncestor,
    };

    enum class WorkspaceGitCommandStatus
    {
        Completed,
        TimedOut,
        LaunchFailed,
        OutputLimitExceeded,
    };

    struct WorkspaceGitCommandResult
    {
        WorkspaceGitCommandStatus Status{ WorkspaceGitCommandStatus::LaunchFailed };
        uint32_t ExitCode{};
        std::string Stdout;
        std::string Stderr;
    };

    using WorkspaceGitCommandRunner = std::function<WorkspaceGitCommandResult(
        WorkspaceGitCommand command,
        const std::filesystem::path& workingDirectory,
        std::chrono::milliseconds timeout,
        std::wstring_view objectId)>;

    struct WorkspaceGitBinding
    {
        // RepositoryRoot is the parent of CommonGitDirectory. For a linked
        // worktree this is the main worktree, while WorktreePath identifies
        // the linked worktree returned by --show-toplevel.
        std::filesystem::path RepositoryRoot;
        std::filesystem::path WorktreePath;
        std::filesystem::path CommonGitDirectory;
        std::filesystem::path GitDirectory;
        winrt::hstring BranchRef;
        winrt::hstring Head;
        // UTC Unix timestamp in milliseconds. It is informational and does
        // not participate in repository/worktree validation equality.
        int64_t CapturedAt{};
    };

    enum class WorkspaceGitMetadataStatus
    {
        Succeeded,
        MissingWorktree,
        TimedOut,
        Unverified,
        Invalid,
    };

    struct WorkspaceGitMetadataResult
    {
        WorkspaceGitMetadataStatus Status{ WorkspaceGitMetadataStatus::Unverified };
        WorkspaceGitBinding Binding;
    };

    enum class WorkspaceGitValidationStatus
    {
        ValidExact,
        SameRepositoryHeadAdvanced,
        BranchMismatch,
        HeadDiverged,
        WorktreeMismatch,
        MissingWorktree,
        DifferentRepository,
        Unverified,
        Invalid,
    };

    struct WorkspaceGitValidationResult
    {
        WorkspaceGitValidationStatus Status{ WorkspaceGitValidationStatus::Unverified };
        WorkspaceGitBinding Current;
    };

    // Reads repository identity without mutating Git state. The injected seam
    // receives an enum rather than arbitrary arguments so production and tests
    // share the same fixed query surface.
    class WorkspaceGitMetadata final
    {
    public:
        static constexpr size_t MaximumPathCharacters{ 32767 };
        static constexpr size_t MaximumRefCharacters{ 1024 };
        static constexpr size_t MaximumStdoutBytes{ 64 * 1024 };
        static constexpr size_t MaximumStderrBytes{ 16 * 1024 };
        static constexpr int64_t MaximumUnixMilliseconds{ 253402300799999 }; // 9999-12-31T23:59:59.999Z
        static constexpr std::chrono::milliseconds DefaultTimeout{ std::chrono::seconds{ 2 } };
        static constexpr std::chrono::milliseconds MaximumTimeout{ std::chrono::seconds{ 10 } };

        explicit WorkspaceGitMetadata(WorkspaceGitCommandRunner runner = {});

        [[nodiscard]] WorkspaceGitMetadataResult Query(
            const std::filesystem::path& path,
            std::chrono::milliseconds timeout = DefaultTimeout) const noexcept;

        [[nodiscard]] WorkspaceGitValidationResult Validate(
            const WorkspaceGitBinding& binding,
            const std::filesystem::path& path,
            std::chrono::milliseconds timeout = DefaultTimeout) const noexcept;

        [[nodiscard]] static bool IsValidBinding(const WorkspaceGitBinding& binding) noexcept;

    private:
        WorkspaceGitCommandRunner _runner;
    };
}
