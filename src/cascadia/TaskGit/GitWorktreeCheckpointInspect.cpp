// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeCheckpoint.h"

#include "GitSourceCommandOutput.h"
#include "GitSourceDigest.h"
#include "GitSourceStatus.h"
#include "GitWorktreeCheckpointInternal.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        GitWorktreeCheckpointStartResult _startFailure(
            const GitWorktreeCheckpointStatus status,
            const DWORD systemError,
            std::string error,
            const GitWorktreeLayout* layout = nullptr)
        {
            GitWorktreeCheckpointStartResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            if (layout)
            {
                result.Preserved = true;
                result.PreservedPath = layout->WorktreePathUtf8;
            }
            return result;
        }

        GitWorktreeCheckpointInspectionResult _inspectionFailure(
            const GitWorktreeCheckpointStatus status,
            const DWORD systemError,
            std::string error,
            const GitWorktreeLayout* layout = nullptr)
        {
            GitWorktreeCheckpointInspectionResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            if (layout)
            {
                result.Preserved = true;
                result.PreservedPath = layout->WorktreePathUtf8;
            }
            return result;
        }

        template<typename TResult>
        TResult _contextFailure(
            const details::GitWorktreeCheckpointContextResult& context,
            const GitWorktreeLayout& layout,
            TResult (*failure)(
                GitWorktreeCheckpointStatus,
                DWORD,
                std::string,
                const GitWorktreeLayout*))
        {
            return failure(
                context.Status,
                context.SystemError,
                context.Error,
                context.Status == GitWorktreeCheckpointStatus::InvalidRequest ?
                    nullptr : &layout);
        }

        template<typename TResult>
        TResult _commandFailure(
            const details::GitWorktreeCheckpointCommandContext& context,
            const GitWorktreeLayout& layout,
            TResult (*failure)(
                GitWorktreeCheckpointStatus,
                DWORD,
                std::string,
                const GitWorktreeLayout*))
        {
            return failure(
                context.FailureStatus,
                context.FailureSystemError,
                context.FailureError,
                &layout);
        }

        bool _verifyCurrentBranch(
            details::GitWorktreeCheckpointCommandContext& context,
            const GitWorktreeLayout& layout,
            const std::string_view expectedHead)
        {
            const auto head = context.Run({ L"rev-parse", L"--verify", L"HEAD" });
            const auto branch = context.Run({
                L"rev-parse",
                L"--verify",
                details::WidenGitSourceAscii(layout.BranchRef),
            });
            const auto headLine = head ? details::ParseGitSourceLine(*head) :
                                         std::optional<std::string>{};
            const auto branchLine = branch ? details::ParseGitSourceLine(*branch) :
                                             std::optional<std::string>{};
            return head && branch && head->ExitCode == 0 && branch->ExitCode == 0 &&
                   headLine == expectedHead && branchLine == expectedHead;
        }
    }

    GitWorktreeCheckpointManager::GitWorktreeCheckpointManager(
        std::filesystem::path gitExecutable,
        GitCommandExecutor executor) :
        _gitExecutable{ std::move(gitExecutable) },
        _executor{ executor ?
                       std::move(executor) :
                       GitCommandExecutor{ [](const GitCommandRequest& request) {
                           return GitCommandRunner{}.Run(request);
                       } } }
    {
    }

    GitWorktreeCheckpointStartResult
    GitWorktreeCheckpointManager::CaptureStart(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) const noexcept
    {
        try
        {
            auto opened = details::OpenGitWorktreeCheckpointContext(
                _gitExecutable,
                _executor,
                layout,
                marker,
                journal,
                cancellationEvent,
                timeoutMilliseconds);
            if (!opened.Context)
            {
                return _contextFailure(
                    opened,
                    layout,
                    _startFailure);
            }
            auto& context = *opened.Context;
            const auto baselineAncestor = context.Run({
                L"merge-base",
                L"--is-ancestor",
                details::WidenGitSourceAscii(marker.BaselineCommit),
                L"HEAD",
            });
            const auto status = context.Run({
                L"status",
                L"--porcelain=v2",
                L"-z",
                L"--untracked-files=all",
            });
            if (!baselineAncestor || !status)
            {
                return _commandFailure(context, layout, _startFailure);
            }
            const auto parsedStatus = details::ParseGitStatusPorcelainV2(
                status->Stdout);
            const auto digest = details::ComputeGitSourceStatusDigest(
                status->Stdout);
            if (baselineAncestor->ExitCode != 0 || status->ExitCode != 0 ||
                !parsedStatus.Succeeded || !digest || !status->Stdout.empty() ||
                !_verifyCurrentBranch(context, layout, context.Target.Head))
            {
                return _startFailure(
                    GitWorktreeCheckpointStatus::TargetChanged,
                    ERROR_REVISION_MISMATCH,
                    "Task Worktree must be clean and retain its managed history before a TaskRun starts",
                    &layout);
            }

            GitWorktreeCheckpointStartResult result;
            result.Status = GitWorktreeCheckpointStatus::Succeeded;
            result.StartCommit = context.Target.Head;
            result.StateDigest = *digest;
            result.DiffSummaryJson =
                "{\"startStateDigest\":\"" + *digest + "\"}";
            return result;
        }
        catch (...)
        {
            return _startFailure(
                GitWorktreeCheckpointStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree checkpoint start capture exhausted local resources");
        }
    }

    GitWorktreeCheckpointInspectionResult
    GitWorktreeCheckpointManager::InspectAndStage(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal,
        const std::string_view startCommit,
        const std::string_view startStateDigest,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) const noexcept
    {
        try
        {
            if (!details::IsGitSourceObjectId(startCommit, journal.ObjectFormat) ||
                startStateDigest.size() != 64)
            {
                return _inspectionFailure(
                    GitWorktreeCheckpointStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Task Worktree checkpoint start evidence is invalid");
            }
            auto opened = details::OpenGitWorktreeCheckpointContext(
                _gitExecutable,
                _executor,
                layout,
                marker,
                journal,
                cancellationEvent,
                timeoutMilliseconds);
            if (!opened.Context)
            {
                return _contextFailure(
                    opened,
                    layout,
                    _inspectionFailure);
            }
            auto& context = *opened.Context;
            const auto startAncestor = context.Run({
                L"merge-base",
                L"--is-ancestor",
                details::WidenGitSourceAscii(startCommit),
                L"HEAD",
            });
            if (!startAncestor)
            {
                return _commandFailure(context, layout, _inspectionFailure);
            }
            if (startAncestor->ExitCode != 0)
            {
                return _inspectionFailure(
                    GitWorktreeCheckpointStatus::TargetChanged,
                    ERROR_REVISION_MISMATCH,
                    "TaskRun rewrote or abandoned its recorded start history",
                    &layout);
            }

            const auto staged = context.Run({
                L"-c",
                L"core.hooksPath=NUL",
                L"add",
                L"-A",
                L"--",
            });
            if (!staged)
            {
                return _commandFailure(context, layout, _inspectionFailure);
            }
            if (staged->ExitCode != 0)
            {
                return _inspectionFailure(
                    GitWorktreeCheckpointStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Git could not stage all TaskRun output",
                    &layout);
            }

            const auto dirty = context.Run({
                L"diff",
                L"--cached",
                L"--quiet",
                L"--exit-code",
                L"HEAD",
                L"--",
            });
            const auto status = context.Run({
                L"status",
                L"--porcelain=v2",
                L"-z",
                L"--untracked-files=all",
            });
            const auto paths = context.Run({
                L"diff",
                L"--cached",
                L"--name-only",
                L"-z",
                details::WidenGitSourceAscii(startCommit),
                L"--",
            });
            const auto numstat = context.Run({
                L"diff",
                L"--cached",
                L"--numstat",
                L"-z",
                details::WidenGitSourceAscii(startCommit),
                L"--",
            });
            if (!dirty || !status || !paths || !numstat)
            {
                return _commandFailure(context, layout, _inspectionFailure);
            }
            if ((dirty->ExitCode != 0 && dirty->ExitCode != 1) ||
                status->ExitCode != 0 || paths->ExitCode != 0 ||
                numstat->ExitCode != 0)
            {
                return _inspectionFailure(
                    GitWorktreeCheckpointStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Task Worktree checkpoint inspection command failed",
                    &layout);
            }

            const auto parsedStatus = details::ParseGitStatusPorcelainV2(
                status->Stdout);
            const auto parsedPaths = details::ParseGitNulPathList(
                paths->Stdout,
                MaximumGitWorktreeCheckpointChangedPaths);
            const auto parsedSummary = details::ParseGitWorktreeCheckpointNumstat(
                numstat->Stdout);
            const auto endDigest = details::ComputeGitSourceStatusDigest(
                status->Stdout);
            if (!parsedStatus.Succeeded || !parsedPaths.Succeeded ||
                !parsedSummary || !endDigest)
            {
                return _inspectionFailure(
                    parsedPaths.CountExceeded ?
                        GitWorktreeCheckpointStatus::OutputLimitExceeded :
                        GitWorktreeCheckpointStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Task Worktree checkpoint diff evidence is invalid or oversized",
                    &layout);
            }
            const auto pathsJson = details::EncodeGitWorktreeCheckpointPaths(
                parsedPaths.Paths);
            const auto summaryJson = details::EncodeGitWorktreeCheckpointSummary(
                startStateDigest,
                *endDigest,
                *parsedSummary);
            const auto needsCommit = dirty->ExitCode == 1;
            if (!pathsJson || !summaryJson ||
                (needsCommit && parsedPaths.Paths.empty()) ||
                (!needsCommit && !status->Stdout.empty()) ||
                !_verifyCurrentBranch(context, layout, context.Target.Head))
            {
                return _inspectionFailure(
                    !pathsJson || !summaryJson ?
                        GitWorktreeCheckpointStatus::OutputLimitExceeded :
                        GitWorktreeCheckpointStatus::TargetChanged,
                    ERROR_REVISION_MISMATCH,
                    "Task Worktree changed while checkpoint evidence was captured",
                    &layout);
            }

            GitWorktreeCheckpointInspectionResult result;
            result.Status = GitWorktreeCheckpointStatus::Succeeded;
            result.AgentEndCommit = context.Target.Head;
            result.ChangedPathsJson = *pathsJson;
            result.DiffSummaryJson = *summaryJson;
            result.NeedsCommit = needsCommit;
            return result;
        }
        catch (...)
        {
            return _inspectionFailure(
                GitWorktreeCheckpointStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree checkpoint inspection exhausted local resources");
        }
    }
}
