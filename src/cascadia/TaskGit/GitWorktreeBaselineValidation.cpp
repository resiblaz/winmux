// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeBaselineInternal.h"

#include "GitSourceCommandOutput.h"
#include "GitWorktreeBaselineProof.h"
#include "GitWorktreeReplayInternal.h"
#include "GitWorktreeReplayProof.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        GitWorktreeBaselineTreeValidationResult _failure(
            GitWorktreeBaselineStatus status,
            DWORD systemError,
            std::string error)
        {
            GitWorktreeBaselineTreeValidationResult result;
            result.Failure = GitWorktreeBaselineFailure(
                status,
                systemError,
                std::move(error));
            return result;
        }

        std::optional<GitWorktreeTreeDiffParseResult> _additions(
            const std::string_view candidateTree,
            const GitWorktreeSourceJournal& journal,
            const GitSourceCommandInvoker& invoke,
            GitWorktreeBaselineTreeValidationResult& failure)
        {
            const auto abbreviation =
                journal.ObjectFormat ==
                        Microsoft::Terminal::TaskExecution::GitObjectFormat::Sha1 ?
                    L"--abbrev=40" :
                    L"--abbrev=64";
            const auto diff = invoke({
                L"diff",
                L"--raw",
                L"--no-renames",
                abbreviation,
                L"-z",
                WidenGitSourceAscii(journal.WorktreeTree),
                WidenGitSourceAscii(candidateTree),
                L"--",
            });
            if (!diff)
            {
                failure = _failure(
                    GitWorktreeBaselineStatus::GitFailure,
                    ERROR_GEN_FAILURE,
                    "Task Worktree baseline tree diff command is unavailable");
                return std::nullopt;
            }
            if (diff->ExitCode != 0)
            {
                failure = _failure(
                    GitWorktreeBaselineStatus::GitFailure,
                    diff->ExitCode,
                    "Task Worktree baseline tree diff command failed");
                return std::nullopt;
            }
            auto parsed = ParseGitWorktreeTreeAdditions(
                diff->Stdout,
                journal.ObjectFormat);
            if (!parsed.Succeeded)
            {
                failure = _failure(
                    GitWorktreeBaselineStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    parsed.Error);
                return std::nullopt;
            }
            return parsed;
        }

        GitWorktreeBaselineTreeValidationResult _verifyFiles(
            GitWorktreeBaselineTreeValidationResult result,
            const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
                targetRoot,
            const GitWorktreeSourceJournal& journal,
            const HANDLE cancellationEvent)
        {
            const auto files = VerifyGitWorktreeReplayFiles(
                targetRoot,
                journal,
                cancellationEvent);
            if (!files.Succeeded())
            {
                result.Succeeded = false;
                result.Failure = GitWorktreeBaselineFailure(
                    files.Status == GitWorktreeReplayStatus::Cancelled ?
                        GitWorktreeBaselineStatus::Cancelled :
                    files.Status == GitWorktreeReplayStatus::ResourceFailure ?
                        GitWorktreeBaselineStatus::ResourceFailure :
                        GitWorktreeBaselineStatus::VerificationFailed,
                    files.SystemError,
                    files.Error);
            }
            return result;
        }
    }

    GitWorktreeBaselineTreeValidationResult ValidateGitWorktreeStagedBaseline(
        const GitWorktreeReplayProof& proof,
        const GitWorktreeSourceJournal& journal,
        const GitSourceCommandInvoker& invoke,
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            targetRoot,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            GitWorktreeBaselineTreeValidationResult result;
            const auto additions = _additions(
                proof.Tracked.IndexTree,
                journal,
                invoke,
                result);
            if (!additions)
            {
                return result;
            }
            if (!GitWorktreeBaselineCandidateMatches(
                    proof,
                    journal,
                    additions->Additions))
            {
                return _failure(
                    GitWorktreeBaselineStatus::TargetChanged,
                    ERROR_REVISION_MISMATCH,
                    "Task Worktree staged baseline contains unexpected changes");
            }
            result.Succeeded = true;
            result.Tree = proof.Tracked.IndexTree;
            return _verifyFiles(
                std::move(result),
                targetRoot,
                journal,
                cancellationEvent);
        }
        catch (...)
        {
            return _failure(
                GitWorktreeBaselineStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree staged baseline validation exhausted local resources");
        }
    }

    GitWorktreeBaselineTreeValidationResult ValidateGitWorktreeCommittedBaseline(
        const std::string_view commit,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal,
        const GitSourceCommandInvoker& invoke,
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            targetRoot,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            const auto subject = invoke({
                L"show",
                L"-s",
                L"--format=%s",
                WidenGitSourceAscii(commit),
            });
            const auto parents = invoke({
                L"rev-list",
                L"--parents",
                L"-n",
                L"1",
                WidenGitSourceAscii(commit),
            });
            const auto tree = invoke({
                L"rev-parse",
                L"--verify",
                WidenGitSourceAscii(commit) + L"^{tree}",
            });
            if (!subject || !parents || !tree)
            {
                return _failure(
                    GitWorktreeBaselineStatus::GitFailure,
                    ERROR_GEN_FAILURE,
                    "Task Worktree baseline commit proof is unavailable");
            }
            const auto subjectLine = ParseGitSourceLine(*subject);
            const auto treeLine = ParseGitSourceLine(*tree);
            if (subject->ExitCode != 0 || parents->ExitCode != 0 ||
                tree->ExitCode != 0 || !subjectLine || !treeLine ||
                *subjectLine != GitWorktreeBaselineSubject(marker) ||
                !IsGitSourceObjectId(*treeLine, journal.ObjectFormat) ||
                !ValidateGitWorktreeBaselineParents(
                    parents->Stdout,
                    commit,
                    journal.SourceHead,
                    journal.ObjectFormat))
            {
                return _failure(
                    GitWorktreeBaselineStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Task Worktree baseline commit ownership proof is invalid");
            }
            const auto proof = CaptureGitWorktreeReplayProof(
                journal.ObjectFormat,
                invoke);
            if (!proof.Succeeded() || proof.Proof->Status.HasStagedChanges ||
                proof.Proof->Status.HasUnstagedChanges ||
                proof.Proof->Status.UntrackedFiles != 0 ||
                proof.Proof->Status.IgnoredFiles != 0 ||
                proof.Proof->Tracked.WorktreeTree != *treeLine ||
                proof.Proof->Tracked.IndexTree != *treeLine)
            {
                return _failure(
                    proof.Status == GitWorktreeReplayProofStatus::ResourceFailure ?
                        GitWorktreeBaselineStatus::ResourceFailure :
                        GitWorktreeBaselineStatus::VerificationFailed,
                    proof.SystemError == ERROR_SUCCESS ?
                        ERROR_REVISION_MISMATCH :
                        proof.SystemError,
                    "Task Worktree baseline commit is not the clean checked-out state");
            }

            GitWorktreeBaselineTreeValidationResult result;
            const auto additions = _additions(
                *treeLine,
                journal,
                invoke,
                result);
            if (!additions)
            {
                return result;
            }
            if (!GitWorktreeBaselineAdditionsMatch(
                    journal,
                    additions->Additions))
            {
                return _failure(
                    GitWorktreeBaselineStatus::VerificationFailed,
                    ERROR_CRC,
                    "Task Worktree baseline commit tree differs from its source snapshot");
            }
            result.Succeeded = true;
            result.Tree = *treeLine;
            return _verifyFiles(
                std::move(result),
                targetRoot,
                journal,
                cancellationEvent);
        }
        catch (...)
        {
            return _failure(
                GitWorktreeBaselineStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree baseline commit validation exhausted local resources");
        }
    }
}
