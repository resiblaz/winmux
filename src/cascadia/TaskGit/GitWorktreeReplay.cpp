// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeReplay.h"

#include "GitSourceCommandOutput.h"
#include "GitWorktreeFileCopyInternal.h"
#include "GitWorktreePreflightInternal.h"
#include "GitWorktreeReplayInternal.h"
#include "GitWorktreeReplayProof.h"

#include "GitCommandRunner.h"
#include "GitRepositoryLock.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        constexpr size_t ReplayStdoutBytes{
            MaximumGitOutputBytes - 64 * 1024
        };
        constexpr size_t ReplayStderrBytes{ 64 * 1024 };

        constexpr auto _failure = details::GitWorktreeReplayFailure;
        constexpr auto _inspectionStatus =
            details::MapGitWorktreeReplayInspectionStatus;
        constexpr auto _lockStatus = details::MapGitWorktreeReplayLockStatus;
        constexpr auto _commandStatus = details::MapGitWorktreeReplayCommandStatus;
        constexpr auto _preflightCommandStatus =
            details::MapGitWorktreeReplayPreflightCommandStatus;
        constexpr auto _widePath = details::DecodeGitWorktreeReplayPath;
        constexpr auto _copyFailure = details::GitWorktreeReplayCopyFailure;

        GitWorktreeReplayResult _preserved(
            GitWorktreeReplayResult result,
            const GitWorktreeLayout& layout,
            const bool trackedApplied = false,
            const size_t copied = 0,
            const size_t reused = 0)
        {
            return details::PreserveGitWorktreeReplayFailure(
                std::move(result),
                layout,
                trackedApplied,
                copied,
                reused);
        }
    }

    GitWorktreeReplayer::GitWorktreeReplayer(
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

    GitWorktreeReplayResult GitWorktreeReplayer::Replay(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) const noexcept
    {
        try
        {
            if (!_executor || _gitExecutable.empty() || !cancellationEvent ||
                cancellationEvent == INVALID_HANDLE_VALUE ||
                timeoutMilliseconds == 0 ||
                timeoutMilliseconds > MaximumGitWorktreeReplayTimeoutMilliseconds)
            {
                return _failure(
                    GitWorktreeReplayStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Task Worktree replay request is invalid");
            }
            const auto evidence = details::ValidateGitWorktreeReplayEvidence(
                layout,
                marker,
                journal);
            if (!evidence.Succeeded())
            {
                return evidence;
            }
            const auto sourcePath = _widePath(journal.SourceWorktreePath);
            if (!sourcePath)
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::InvalidRequest,
                        ERROR_BAD_PATHNAME,
                        "Task Worktree replay source path is invalid"),
                    layout);
            }

            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            const auto remaining = [&]() -> std::optional<uint32_t> {
                const auto now = GetTickCount64();
                return now < deadline ?
                           std::optional<uint32_t>{ static_cast<uint32_t>(
                               (std::min<uint64_t>)(deadline - now, UINT32_MAX)) } :
                           std::nullopt;
            };

            GitRepositoryInspector inspector{ _gitExecutable, _executor };
            const auto sourceTime = remaining();
            if (!sourceTime)
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Task Worktree replay expired before source inspection"),
                    layout);
            }
            const auto source = inspector.Inspect(
                *sourcePath,
                cancellationEvent,
                (std::min)(*sourceTime, MaximumGitCommandTimeoutMilliseconds));
            if (!source.Succeeded())
            {
                return _preserved(
                    _failure(
                        _inspectionStatus(source.Status),
                        source.SystemError,
                        "Task Worktree replay source inspection failed"),
                    layout);
            }
            if (source.Inspection->CommonDirectoryIdentity.Digest() !=
                    journal.RepositoryIdentityDigest ||
                source.Inspection->ObjectFormat != journal.ObjectFormat ||
                source.Inspection->Head != journal.SourceHead ||
                source.Inspection->BranchRef != journal.SourceReference)
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::PreflightRejected,
                        ERROR_REVISION_MISMATCH,
                        "Task Worktree replay source no longer matches its journal"),
                    layout);
            }

            const auto lockTime = remaining();
            if (!lockTime)
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Task Worktree replay expired before repository locking"),
                    layout);
            }
            auto gitLock = AcquireGitRepositoryLock(
                source.Inspection->CommonDirectoryIdentity,
                cancellationEvent,
                (std::min)(*lockTime, GitRepositoryLockTimeoutMilliseconds));
            if (!gitLock.Acquired())
            {
                return _preserved(
                    _failure(
                        _lockStatus(gitLock.Status),
                        gitLock.SystemError,
                        "Task Worktree replay common-directory lock failed"),
                    layout);
            }

            const auto preflightTime = remaining();
            if (!preflightTime)
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Task Worktree replay expired before ownership preflight"),
                    layout);
            }
            GitWorktreePreflightInspector preflight{
                _gitExecutable,
                _executor
            };
            const auto ownership = preflight.Inspect(
                layout,
                marker,
                journal,
                cancellationEvent,
                GitWorktreePreflightMode::AllowOwnedRecovery,
                (std::min)(
                    *preflightTime,
                    MaximumGitWorktreePreflightTimeoutMilliseconds));
            if (!ownership.Succeeded() ||
                ownership.Disposition !=
                    GitWorktreePreflightDisposition::ResumeBranch)
            {
                auto result = _failure(
                    ownership.Status == GitWorktreePreflightStatus::Cancelled ?
                        GitWorktreeReplayStatus::Cancelled :
                    ownership.Status == GitWorktreePreflightStatus::TimedOut ?
                        GitWorktreeReplayStatus::TimedOut :
                    ownership.Status == GitWorktreePreflightStatus::ResourceFailure ?
                        GitWorktreeReplayStatus::ResourceFailure :
                        GitWorktreeReplayStatus::PreflightRejected,
                    ownership.SystemError,
                    ownership.Error.empty() ?
                        "Task Worktree replay ownership preflight failed" :
                        ownership.Error);
                result.PreflightStatus = ownership.Status;
                return _preserved(std::move(result), layout);
            }

            const auto targetTime = remaining();
            if (!targetTime)
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Task Worktree replay expired before target inspection"),
                    layout);
            }
            const auto target = inspector.Inspect(
                layout.WorktreePath,
                cancellationEvent,
                (std::min)(*targetTime, MaximumGitCommandTimeoutMilliseconds));
            if (!target.Succeeded() ||
                target.Inspection->CommonDirectoryIdentity.Digest() !=
                    journal.RepositoryIdentityDigest ||
                target.Inspection->ObjectFormat != journal.ObjectFormat ||
                target.Inspection->Head != journal.SourceHead ||
                target.Inspection->BranchRef != layout.BranchRef)
            {
                return _preserved(
                    _failure(
                        target.Succeeded() ?
                            GitWorktreeReplayStatus::TargetChanged :
                            _inspectionStatus(target.Status),
                        target.SystemError == ERROR_SUCCESS ?
                            ERROR_REVISION_MISMATCH :
                            target.SystemError,
                        "Task Worktree replay target identity is invalid"),
                    layout);
            }

            GitWorktreeReplayResult commandFailure;
            const auto run = [&](std::vector<std::wstring> arguments) ->
                std::optional<GitCommandResult> {
                const auto time = remaining();
                if (!time)
                {
                    commandFailure = _failure(
                        GitWorktreeReplayStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Task Worktree replay command deadline expired");
                    return std::nullopt;
                }
                GitCommandRequest request;
                request.GitExecutable = target.Inspection->GitExecutable;
                request.WorkingDirectory = target.Inspection->WorktreeIdentity;
                request.Arguments = std::move(arguments);
                request.CancellationEvent = cancellationEvent;
                request.TimeoutMilliseconds = (std::min)(
                    *time,
                    MaximumGitCommandTimeoutMilliseconds);
                request.MaximumStdoutBytes = ReplayStdoutBytes;
                request.MaximumStderrBytes = ReplayStderrBytes;
                auto result = _executor(request);
                if (result.Status != GitCommandStatus::Completed)
                {
                    commandFailure = _failure(
                        _commandStatus(result.Status),
                        result.SystemError,
                        "Task Worktree replay Git command failed");
                    return std::nullopt;
                }
                return result;
            };
            const details::GitSourceCommandInvoker invoke = run;

            const auto headTreeResult = run({
                L"rev-parse",
                L"--verify",
                details::WidenGitSourceAscii(journal.SourceHead) + L"^{tree}",
            });
            if (!headTreeResult)
            {
                return _preserved(commandFailure, layout);
            }
            const auto headTree = details::ParseGitSourceLine(*headTreeResult);
            if (headTreeResult->ExitCode != 0 || !headTree ||
                !details::IsGitSourceObjectId(*headTree, journal.ObjectFormat))
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::VerificationFailed,
                        ERROR_INVALID_DATA,
                        "Task Worktree replay HEAD tree is invalid"),
                    layout);
            }

            auto proof = details::CaptureGitWorktreeReplayProof(
                journal.ObjectFormat,
                invoke);
            if (!proof.Succeeded())
            {
                return _preserved(
                    details::GitWorktreeReplayProofFailure(
                        proof,
                        commandFailure),
                    layout);
            }

            bool trackedApplied{};
            if (!details::GitWorktreeReplayTrackedMatches(
                    *proof.Proof,
                    journal))
            {
                if (!details::GitWorktreeReplayTrackedIsPristine(
                        *proof.Proof,
                        *headTree) ||
                    journal.TrackedSnapshotCommit.empty())
                {
                    return _preserved(
                        _failure(
                            GitWorktreeReplayStatus::TargetChanged,
                            ERROR_REVISION_MISMATCH,
                            "Task Worktree tracked state is neither pristine nor replayed"),
                        layout);
                }
                const auto applied = run({
                    L"-c",
                    L"core.hooksPath=NUL",
                    L"stash",
                    L"apply",
                    L"--index",
                    details::WidenGitSourceAscii(
                        journal.TrackedSnapshotCommit),
                });
                if (!applied)
                {
                    return _preserved(commandFailure, layout);
                }
                if (applied->ExitCode != 0)
                {
                    return _preserved(
                        _failure(
                            GitWorktreeReplayStatus::TrackedApplyFailed,
                            ERROR_INVALID_DATA,
                            "Git could not apply the captured tracked state"),
                        layout);
                }
                trackedApplied = true;
                proof = details::CaptureGitWorktreeReplayProof(
                    journal.ObjectFormat,
                    invoke);
                if (!proof.Succeeded() ||
                    !details::GitWorktreeReplayTrackedMatches(
                        *proof.Proof,
                        journal))
                {
                    return _preserved(
                        proof.Succeeded() ?
                            _failure(
                                GitWorktreeReplayStatus::VerificationFailed,
                                ERROR_CRC,
                                "Applied Task Worktree tracked state differs from its proof") :
                            details::GitWorktreeReplayProofFailure(
                                proof,
                                commandFailure),
                        layout,
                        trackedApplied);
                }
            }

            const auto stagingCleanup =
                details::CleanupGitWorktreeCopyStaging(layout.CopyStagingPath);
            if (!stagingCleanup.Succeeded())
            {
                return _copyFailure(
                    stagingCleanup,
                    layout,
                    {},
                    trackedApplied,
                    0,
                    0);
            }

            size_t copied{};
            size_t reused{};
            for (const auto& file : journal.UntrackedFiles)
            {
                if (!remaining())
                {
                    return _preserved(
                        _failure(
                            GitWorktreeReplayStatus::TimedOut,
                            WAIT_TIMEOUT,
                            "Task Worktree replay expired during file copy"),
                        layout,
                        trackedApplied,
                        copied,
                        reused);
                }
                const auto result = CopyGitWorktreeSnapshotFile(
                    source.Inspection->WorktreeIdentity,
                    target.Inspection->WorktreeIdentity,
                    layout.StateRoot,
                    layout.CopyStagingPath,
                    file,
                    cancellationEvent);
                if (!result.Succeeded())
                {
                    return _copyFailure(
                        result,
                        layout,
                        file.RelativePath,
                        trackedApplied,
                        copied,
                        reused);
                }
                result.Copied() ? ++copied : ++reused;
            }

            const auto finalProof = details::CaptureGitWorktreeReplayProof(
                journal.ObjectFormat,
                invoke);
            if (!finalProof.Succeeded())
            {
                return _preserved(
                    details::GitWorktreeReplayProofFailure(
                        finalProof,
                        commandFailure),
                    layout,
                    trackedApplied,
                    copied,
                    reused);
            }
            if (!details::GitWorktreeReplayTrackedMatches(
                    *finalProof.Proof,
                    journal) ||
                !details::GitWorktreeReplayManifestMatches(
                    *finalProof.Proof,
                    journal))
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::VerificationFailed,
                        ERROR_CRC,
                        "Task Worktree final tree or manifest proof does not match"),
                    layout,
                    trackedApplied,
                    copied,
                    reused);
            }
            const auto verifiedFiles = details::VerifyGitWorktreeReplayFiles(
                target.Inspection->WorktreeIdentity,
                journal,
                cancellationEvent);
            if (!verifiedFiles.Succeeded())
            {
                auto result = verifiedFiles;
                result.FailurePath = verifiedFiles.FailurePath;
                return _preserved(
                    std::move(result),
                    layout,
                    trackedApplied,
                    copied,
                    reused);
            }

            const details::GitWorktreePreflightCommandInvoker collisionInvoker =
                [&](std::vector<std::wstring> arguments) {
                details::GitWorktreePreflightCommandInvocation invocation;
                const auto command = run(std::move(arguments));
                if (!command)
                {
                    invocation.Failure.Status = _preflightCommandStatus(
                        commandFailure.Status);
                    invocation.Failure.SystemError = commandFailure.SystemError;
                    invocation.Failure.Error = commandFailure.Error;
                    return invocation;
                }
                invocation.Result = *command;
                return invocation;
            };
            const auto finalOwnership = details::InspectGitWorktreeCollision(
                layout,
                journal,
                GitWorktreePreflightMode::AllowOwnedRecovery,
                collisionInvoker);
            if (!finalOwnership.Succeeded() ||
                finalOwnership.Disposition !=
                    GitWorktreePreflightDisposition::ResumeBranch)
            {
                auto result = _failure(
                    GitWorktreeReplayStatus::VerificationFailed,
                    finalOwnership.SystemError,
                    "Task Worktree ownership changed during replay");
                result.PreflightStatus = finalOwnership.Status;
                return _preserved(
                    std::move(result),
                    layout,
                    trackedApplied,
                    copied,
                    reused);
            }

            const auto confirmedProof = details::CaptureGitWorktreeReplayProof(
                journal.ObjectFormat,
                invoke);
            if (!confirmedProof.Succeeded())
            {
                return _preserved(
                    details::GitWorktreeReplayProofFailure(
                        confirmedProof,
                        commandFailure),
                    layout,
                    trackedApplied,
                    copied,
                    reused);
            }
            if (!details::GitWorktreeReplayTrackedMatches(
                    *confirmedProof.Proof,
                    journal) ||
                !details::GitWorktreeReplayManifestMatches(
                    *confirmedProof.Proof,
                    journal))
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::VerificationFailed,
                        ERROR_REVISION_MISMATCH,
                        "Task Worktree changed during final replay verification"),
                    layout,
                    trackedApplied,
                    copied,
                    reused);
            }
            const auto confirmedFiles = details::VerifyGitWorktreeReplayFiles(
                target.Inspection->WorktreeIdentity,
                journal,
                cancellationEvent);
            if (!confirmedFiles.Succeeded())
            {
                return _preserved(
                    confirmedFiles,
                    layout,
                    trackedApplied,
                    copied,
                    reused);
            }
            if (!remaining())
            {
                return _preserved(
                    _failure(
                        GitWorktreeReplayStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Task Worktree replay exceeded its deadline"),
                    layout,
                    trackedApplied,
                    copied,
                    reused);
            }

            GitWorktreeReplayResult result;
            result.Status = GitWorktreeReplayStatus::Succeeded;
            result.TrackedApplied = trackedApplied;
            result.FilesCopied = copied;
            result.FilesReused = reused;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeReplayStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree replay exhausted local resources");
        }
    }
}
