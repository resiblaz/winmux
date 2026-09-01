// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeFinalizeInternal.h"

#include "GitSourceCommandOutput.h"
#include "GitSourceStatus.h"
#include "GitTrackedSource.h"
#include "GitWorktreeBaselineInternal.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        GitWorktreeFinalizeResult _failure(
            const GitWorktreeFinalizeStatus status,
            const DWORD systemError,
            std::string error,
            const GitWorktreeLayout& layout)
        {
            GitWorktreeFinalizeResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            result.Preserved = true;
            result.PreservedPath = layout.WorktreePathUtf8;
            return result;
        }

        GitWorktreeFinalizeResult _commandFailure(
            const GitWorktreeFinalizeCommandContext& context,
            const GitWorktreeLayout& layout)
        {
            return _failure(
                context.FailureStatus,
                context.FailureSystemError,
                context.FailureError,
                layout);
        }

        GitWorktreeFinalizeStatus _policyStatus(
            const GitWorktreeBaselineStatus status) noexcept
        {
            switch (status)
            {
            case GitWorktreeBaselineStatus::InvalidRequest:
                return GitWorktreeFinalizeStatus::InvalidRequest;
            case GitWorktreeBaselineStatus::IdentityFailure:
                return GitWorktreeFinalizeStatus::IdentityFailure;
            case GitWorktreeBaselineStatus::Cancelled:
                return GitWorktreeFinalizeStatus::Cancelled;
            case GitWorktreeBaselineStatus::TimedOut:
                return GitWorktreeFinalizeStatus::TimedOut;
            case GitWorktreeBaselineStatus::ResourceFailure:
                return GitWorktreeFinalizeStatus::ResourceFailure;
            default:
                return GitWorktreeFinalizeStatus::GitFailure;
            }
        }

        bool _cleanStatus(
            GitWorktreeFinalizeCommandContext& context,
            const GitRepositoryInspection& target,
            GitWorktreeFinalizeResult& failure,
            const GitWorktreeLayout& layout)
        {
            const auto status = context.Run(
                target.WorktreeIdentity,
                {
                    L"status",
                    L"--porcelain=v2",
                    L"-z",
                    L"--untracked-files=all",
                    L"--ignored=matching",
                });
            if (!status)
            {
                failure = _commandFailure(context, layout);
                return false;
            }
            const auto parsed = ParseGitStatusPorcelainV2(status->Stdout);
            if (status->ExitCode != 0 || !parsed.Succeeded ||
                parsed.Summary.HasStagedChanges ||
                parsed.Summary.HasUnstagedChanges ||
                parsed.Summary.UntrackedFiles != 0 ||
                parsed.Summary.IgnoredFiles != 0)
            {
                failure = _failure(
                    GitWorktreeFinalizeStatus::VerificationFailed,
                    ERROR_DIR_NOT_EMPTY,
                    "Task Worktree is not clean after finalization checkpoint",
                    layout);
                return false;
            }
            return true;
        }

        bool _verifyManagedCommit(
            GitWorktreeFinalizeCommandContext& context,
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker,
            const GitRepositoryInspection& target,
            const std::string_view commit,
            std::string& parent,
            GitWorktreeFinalizeResult& failure)
        {
            const auto subjectCommand = context.Run(
                target.WorktreeIdentity,
                { L"show", L"-s", L"--format=%s", L"HEAD" });
            const auto parentsCommand = context.Run(
                target.WorktreeIdentity,
                { L"rev-list", L"--parents", L"-n", L"1", L"HEAD" });
            const auto branchCommand = context.Run(
                target.WorktreeIdentity,
                {
                    L"rev-parse",
                    L"--verify",
                    WidenGitSourceAscii(layout.BranchRef),
                });
            if (!subjectCommand || !parentsCommand || !branchCommand)
            {
                failure = _commandFailure(context, layout);
                return false;
            }
            const auto subject = ParseGitSourceLine(*subjectCommand);
            const auto parents = ParseGitSourceLine(*parentsCommand);
            const auto branch = ParseGitSourceLine(*branchCommand);
            if (subjectCommand->ExitCode != 0 || parentsCommand->ExitCode != 0 ||
                branchCommand->ExitCode != 0 || !subject || !parents || !branch ||
                *subject != GitWorktreeFinalizeSubject(marker) ||
                *branch != commit || !parents->starts_with(std::string{ commit } + " "))
            {
                failure = _failure(
                    GitWorktreeFinalizeStatus::VerificationFailed,
                    ERROR_REVISION_MISMATCH,
                    "Task Worktree finalization commit ownership proof is invalid",
                    layout);
                return false;
            }
            const auto separator = parents->find(' ');
            if (separator == std::string::npos ||
                parents->find(' ', separator + 1) != std::string::npos)
            {
                failure = _failure(
                    GitWorktreeFinalizeStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Task Worktree finalization commit parent proof is invalid",
                    layout);
                return false;
            }
            parent = parents->substr(separator + 1);
            if (!IsGitSourceObjectId(parent, target.ObjectFormat) || parent == commit)
            {
                failure = _failure(
                    GitWorktreeFinalizeStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Task Worktree finalization commit parent is invalid",
                    layout);
                return false;
            }
            const auto nonempty = context.Run(
                target.WorktreeIdentity,
                {
                    L"diff-tree",
                    L"--quiet",
                    L"--exit-code",
                    WidenGitSourceAscii(parent),
                    WidenGitSourceAscii(std::string{ commit }),
                    L"--",
                });
            if (!nonempty)
            {
                failure = _commandFailure(context, layout);
                return false;
            }
            if (nonempty->ExitCode != 1 ||
                !_cleanStatus(context, target, failure, layout))
            {
                if (nonempty->ExitCode != 1)
                {
                    failure = _failure(
                        GitWorktreeFinalizeStatus::VerificationFailed,
                        ERROR_INVALID_DATA,
                        "Task Worktree finalization commit is empty",
                        layout);
                }
                return false;
            }
            return true;
        }
    }

    GitWorktreeFinalizeCheckpointResult CheckpointGitWorktreeForFinalization(
        GitWorktreeFinalizeCommandContext& context,
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspection& target) noexcept
    {
        GitWorktreeFinalizeCheckpointResult result;
        try
        {
            const auto status = context.Run(
                target.WorktreeIdentity,
                {
                    L"status",
                    L"--porcelain=v2",
                    L"-z",
                    L"--untracked-files=all",
                    L"--ignored=matching",
                });
            if (!status)
            {
                result.Failure = _commandFailure(context, layout);
                return result;
            }
            const auto parsedStatus = ParseGitStatusPorcelainV2(status->Stdout);
            if (status->ExitCode != 0 || !parsedStatus.Succeeded)
            {
                result.Failure = _failure(
                    GitWorktreeFinalizeStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Task Worktree finalization status is invalid",
                    layout);
                return result;
            }
            if (parsedStatus.Summary.IgnoredFiles != 0)
            {
                result.Failure = _failure(
                    GitWorktreeFinalizeStatus::TargetChanged,
                    ERROR_DIR_NOT_EMPTY,
                    "Task Worktree contains ignored files that cannot be delivered safely",
                    layout);
                return result;
            }

            auto finalCommit = target.Head;
            const auto dirty = parsedStatus.Summary.HasStagedChanges ||
                               parsedStatus.Summary.HasUnstagedChanges ||
                               parsedStatus.Summary.UntrackedFiles != 0;
            if (!dirty)
            {
                const auto subjectCommand = context.Run(
                    target.WorktreeIdentity,
                    { L"show", L"-s", L"--format=%s", L"HEAD" });
                if (!subjectCommand)
                {
                    result.Failure = _commandFailure(context, layout);
                    return result;
                }
                const auto subject = ParseGitSourceLine(*subjectCommand);
                if (subjectCommand->ExitCode != 0 || !subject)
                {
                    result.Failure = _failure(
                        GitWorktreeFinalizeStatus::VerificationFailed,
                        ERROR_INVALID_DATA,
                        "Task Worktree finalization HEAD subject is invalid",
                        layout);
                    return result;
                }
                if (*subject == GitWorktreeFinalizeSubject(marker))
                {
                    std::string parent;
                    if (!_verifyManagedCommit(
                            context,
                            layout,
                            marker,
                            target,
                            finalCommit,
                            parent,
                            result.Failure))
                    {
                        return result;
                    }
                    result.CheckpointCommit = finalCommit;
                }
                result.Succeeded = true;
                result.FinalCommit = std::move(finalCommit);
                return result;
            }

            const auto staged = context.Run(
                target.WorktreeIdentity,
                { L"-c", L"core.hooksPath=NUL", L"add", L"-A", L"--" });
            const auto nonempty = context.Run(
                target.WorktreeIdentity,
                {
                    L"diff",
                    L"--cached",
                    L"--quiet",
                    L"--exit-code",
                    L"HEAD",
                    L"--",
                });
            if (!staged || !nonempty)
            {
                result.Failure = _commandFailure(context, layout);
                return result;
            }
            if (staged->ExitCode != 0 || nonempty->ExitCode != 1)
            {
                result.Failure = _failure(
                    GitWorktreeFinalizeStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Git could not stage a non-empty finalization checkpoint",
                    layout);
                return result;
            }
            const auto stagedStatus = context.Run(
                target.WorktreeIdentity,
                {
                    L"status",
                    L"--porcelain=v2",
                    L"-z",
                    L"--untracked-files=all",
                    L"--ignored=matching",
                });
            if (!stagedStatus)
            {
                result.Failure = _commandFailure(context, layout);
                return result;
            }
            const auto stagedSummary = ParseGitStatusPorcelainV2(
                stagedStatus->Stdout);
            if (stagedStatus->ExitCode != 0 || !stagedSummary.Succeeded ||
                !stagedSummary.Summary.HasStagedChanges ||
                stagedSummary.Summary.HasUnstagedChanges ||
                stagedSummary.Summary.UntrackedFiles != 0 ||
                stagedSummary.Summary.IgnoredFiles != 0)
            {
                result.Failure = _failure(
                    GitWorktreeFinalizeStatus::TargetChanged,
                    ERROR_REVISION_MISMATCH,
                    "Task Worktree changed while finalization staged its output",
                    layout);
                return result;
            }

            const GitSourceCommandInvoker invoke =
                [&](std::vector<std::wstring> arguments) {
                    return context.Run(
                        target.WorktreeIdentity,
                        std::move(arguments));
                };
            const auto policy = InspectGitWorktreeCommitPolicy(invoke);
            if (!policy.Succeeded)
            {
                result.Failure = !context.FailureError.empty() ?
                                     _commandFailure(context, layout) :
                                     _failure(
                                         _policyStatus(policy.Failure.Status),
                                         policy.Failure.SystemError,
                                         policy.Failure.Error,
                                         layout);
                return result;
            }
            result.UsedFallbackIdentity = policy.UseFallbackIdentity;
            result.SigningConfigured = policy.SigningConfigured;
            std::vector<std::wstring> arguments{
                L"-c",
                L"core.hooksPath=NUL",
            };
            if (policy.UseFallbackIdentity)
            {
                arguments.insert(
                    arguments.end(),
                    {
                        L"-c",
                        L"user.name=Richer Task Runtime",
                        L"-c",
                        L"user.email=task-runtime@localhost",
                    });
            }
            arguments.insert(
                arguments.end(),
                {
                    L"commit",
                    L"--no-verify",
                    L"-m",
                    WidenGitSourceAscii(GitWorktreeFinalizeSubject(marker)),
                });
            const auto committed = context.Run(
                target.WorktreeIdentity,
                std::move(arguments));
            if (!committed)
            {
                result.Failure = _commandFailure(context, layout);
                result.Failure.UsedFallbackIdentity = policy.UseFallbackIdentity;
                result.Failure.SigningConfigured = policy.SigningConfigured;
                return result;
            }
            if (committed->ExitCode != 0)
            {
                result.Failure = _failure(
                    policy.SigningConfigured ?
                        GitWorktreeFinalizeStatus::SigningFailure :
                        GitWorktreeFinalizeStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    policy.SigningConfigured ?
                        "Configured Git signing failed for finalization" :
                        "Git could not create the finalization checkpoint",
                    layout);
                result.Failure.UsedFallbackIdentity = policy.UseFallbackIdentity;
                result.Failure.SigningConfigured = policy.SigningConfigured;
                return result;
            }
            result.CommitCreated = true;
            const auto head = context.Run(
                target.WorktreeIdentity,
                { L"rev-parse", L"--verify", L"HEAD" });
            const auto parsedHead = head ? ParseGitSourceLine(*head) :
                                           std::optional<std::string>{};
            if (!head || head->ExitCode != 0 || !parsedHead ||
                !IsGitSourceObjectId(*parsedHead, target.ObjectFormat) ||
                *parsedHead == target.Head)
            {
                result.Failure = !head ?
                                     _commandFailure(context, layout) :
                                     _failure(
                                         GitWorktreeFinalizeStatus::VerificationFailed,
                                         ERROR_INVALID_DATA,
                                         "Finalization checkpoint commit ID is invalid",
                                         layout);
                result.Failure.CheckpointCommit = parsedHead.value_or("");
                result.Failure.CheckpointCreated = true;
                return result;
            }
            finalCommit = *parsedHead;
            std::string parent;
            if (!_verifyManagedCommit(
                    context,
                    layout,
                    marker,
                    target,
                    finalCommit,
                    parent,
                    result.Failure) ||
                parent != target.Head)
            {
                if (result.Failure.Error.empty())
                {
                    result.Failure = _failure(
                        GitWorktreeFinalizeStatus::VerificationFailed,
                        ERROR_REVISION_MISMATCH,
                        "Finalization checkpoint parent changed",
                        layout);
                }
                result.Failure.CheckpointCommit = finalCommit;
                result.Failure.CheckpointCreated = true;
                return result;
            }
            result.Succeeded = true;
            result.FinalCommit = finalCommit;
            result.CheckpointCommit = finalCommit;
            return result;
        }
        catch (...)
        {
            result.Failure = _failure(
                GitWorktreeFinalizeStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree finalization checkpoint exhausted local resources",
                layout);
            return result;
        }
    }
}
