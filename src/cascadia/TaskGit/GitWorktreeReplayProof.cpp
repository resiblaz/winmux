// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeReplayProof.h"

#include "GitSourceStatus.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        GitWorktreeReplayProofResult _failure(
            const GitWorktreeReplayProofStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreeReplayProofResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }
    }

    GitWorktreeReplayProofResult CaptureGitWorktreeReplayProof(
        const Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat,
        const GitSourceCommandInvoker& invoke) noexcept
    {
        try
        {
            if (!invoke)
            {
                return _failure(
                    GitWorktreeReplayProofStatus::CommandUnavailable,
                    ERROR_INVALID_PARAMETER,
                    "Task Worktree replay proof command invoker is unavailable");
            }
            const auto statusBefore = invoke({
                L"status",
                L"--porcelain=v2",
                L"-z",
                L"--untracked-files=all",
                L"--ignored=matching",
                L"--no-renames",
            });
            const auto pathsBefore = invoke({
                L"ls-files",
                L"--others",
                L"--exclude-standard",
                L"-z",
            });
            if (!statusBefore || !pathsBefore)
            {
                return _failure(
                    GitWorktreeReplayProofStatus::CommandUnavailable,
                    ERROR_GEN_FAILURE,
                    "Task Worktree replay inventory command is unavailable");
            }
            if (statusBefore->ExitCode != 0 || pathsBefore->ExitCode != 0)
            {
                return _failure(
                    GitWorktreeReplayProofStatus::GitFailure,
                    statusBefore->ExitCode != 0 ?
                        statusBefore->ExitCode :
                        pathsBefore->ExitCode,
                    "Task Worktree replay inventory command failed");
            }
            const auto status = ParseGitStatusPorcelainV2(
                statusBefore->Stdout);
            const auto paths = ParseGitNulPathList(
                pathsBefore->Stdout,
                MaximumGitSnapshotUntrackedFiles);
            if (!status.Succeeded || !paths.Succeeded ||
                status.Summary.UntrackedFiles != paths.Paths.size())
            {
                return _failure(
                    GitWorktreeReplayProofStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Task Worktree replay inventory output is invalid");
            }

            const auto tracked = CaptureGitTrackedSourceProof(
                status.Summary,
                objectFormat,
                invoke);
            if (!tracked.Succeeded())
            {
                return _failure(
                    tracked.Status == GitTrackedSourceProofStatus::CommandUnavailable ?
                        GitWorktreeReplayProofStatus::CommandUnavailable :
                    tracked.Status == GitTrackedSourceProofStatus::GitFailure ?
                        GitWorktreeReplayProofStatus::GitFailure :
                        GitWorktreeReplayProofStatus::InvalidOutput,
                    tracked.SystemError,
                    tracked.Error);
            }

            const auto statusAfter = invoke({
                L"status",
                L"--porcelain=v2",
                L"-z",
                L"--untracked-files=all",
                L"--ignored=matching",
                L"--no-renames",
            });
            const auto pathsAfter = invoke({
                L"ls-files",
                L"--others",
                L"--exclude-standard",
                L"-z",
            });
            if (!statusAfter || !pathsAfter)
            {
                return _failure(
                    GitWorktreeReplayProofStatus::CommandUnavailable,
                    ERROR_GEN_FAILURE,
                    "Task Worktree replay final inventory command is unavailable");
            }
            if (statusAfter->ExitCode != 0 || pathsAfter->ExitCode != 0)
            {
                return _failure(
                    GitWorktreeReplayProofStatus::GitFailure,
                    statusAfter->ExitCode != 0 ?
                        statusAfter->ExitCode :
                        pathsAfter->ExitCode,
                    "Task Worktree replay final inventory command failed");
            }
            if (statusAfter->Stdout != statusBefore->Stdout ||
                pathsAfter->Stdout != pathsBefore->Stdout)
            {
                return _failure(
                    GitWorktreeReplayProofStatus::Changed,
                    ERROR_REVISION_MISMATCH,
                    "Task Worktree changed while replay proof was captured");
            }

            GitWorktreeReplayProofResult result;
            result.Status = GitWorktreeReplayProofStatus::Succeeded;
            result.Proof.emplace();
            result.Proof->Status = status.Summary;
            result.Proof->Tracked = *tracked.Proof;
            result.Proof->UntrackedPaths = paths.Paths;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeReplayProofStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree replay proof exhausted local resources");
        }
    }

    bool GitWorktreeReplayTrackedMatches(
        const GitWorktreeReplayProof& proof,
        const GitWorktreeSourceJournal& journal) noexcept
    {
        return proof.Tracked.WorktreeTree == journal.WorktreeTree &&
               proof.Tracked.IndexTree == journal.IndexTree;
    }

    bool GitWorktreeReplayTrackedIsPristine(
        const GitWorktreeReplayProof& proof,
        const std::string_view headTree) noexcept
    {
        return !proof.Status.HasStagedChanges &&
               !proof.Status.HasUnstagedChanges &&
               proof.Tracked.WorktreeTree == headTree &&
               proof.Tracked.IndexTree == headTree;
    }

    bool GitWorktreeReplayManifestMatches(
        const GitWorktreeReplayProof& proof,
        const GitWorktreeSourceJournal& journal) noexcept
    {
        // Ignored source files are intentionally absent from a Task Worktree.
        // Exact tracked trees plus this non-ignored manifest are the replay
        // proof; the source-only StatusDigest remains capture/recovery evidence.
        if (proof.Status.IgnoredFiles != 0 ||
            proof.UntrackedPaths.size() != journal.UntrackedFiles.size())
        {
            return false;
        }
        for (size_t index = 0; index < proof.UntrackedPaths.size(); ++index)
        {
            if (proof.UntrackedPaths[index] !=
                journal.UntrackedFiles[index].RelativePath)
            {
                return false;
            }
        }
        return true;
    }
}
