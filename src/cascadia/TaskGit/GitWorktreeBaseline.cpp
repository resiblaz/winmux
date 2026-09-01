// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeBaseline.h"

#include "GitSourceCommandOutput.h"
#include "GitWorktreeBaselineInternal.h"
#include "GitWorktreeBaselineProof.h"
#include "GitWorktreeReplayInternal.h"
#include "GitWorktreeReplayProof.h"

#include "GitCommandRunner.h"
#include "GitRepositoryLock.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        constexpr size_t BaselineStdoutBytes{
            MaximumGitOutputBytes - 64 * 1024
        };
        constexpr size_t BaselineStderrBytes{ 64 * 1024 };

        constexpr auto _failure = details::GitWorktreeBaselineFailure;
        constexpr auto _preserved = details::PreserveGitWorktreeBaselineFailure;
        constexpr auto _inspectionStatus =
            details::MapGitWorktreeBaselineInspectionStatus;
        constexpr auto _lockStatus = details::MapGitWorktreeBaselineLockStatus;
        constexpr auto _commandStatus = details::MapGitWorktreeBaselineCommandStatus;

        GitWorktreeBaselineResult _mapEvidenceFailure(
            const GitWorktreeReplayResult& evidence)
        {
            return _failure(
                evidence.Status == GitWorktreeReplayStatus::ResourceFailure ?
                    GitWorktreeBaselineStatus::ResourceFailure :
                evidence.Status == GitWorktreeReplayStatus::InvalidRequest ?
                    GitWorktreeBaselineStatus::InvalidRequest :
                    GitWorktreeBaselineStatus::EvidenceMismatch,
                evidence.SystemError,
                evidence.Error);
        }
    }

    GitWorktreeBaselineCreator::GitWorktreeBaselineCreator(
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

    GitWorktreeBaselineResult GitWorktreeBaselineCreator::Create(
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
                timeoutMilliseconds > MaximumGitWorktreeBaselineTimeoutMilliseconds)
            {
                return _failure(
                    GitWorktreeBaselineStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Task Worktree baseline request is invalid");
            }
            const auto evidence = details::ValidateGitWorktreeReplayEvidence(
                layout,
                marker,
                journal);
            if (!evidence.Succeeded())
            {
                return _mapEvidenceFailure(evidence);
            }

            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            const auto remaining = [&]() -> std::optional<uint32_t> {
                const auto now = GetTickCount64();
                return now < deadline ?
                           std::optional<uint32_t>{ static_cast<uint32_t>(
                               (std::min<uint64_t>)(deadline - now, UINT32_MAX)) } :
                           std::nullopt;
            };
            const auto preservedFailure = [&](GitWorktreeBaselineResult result) {
                return _preserved(std::move(result), layout);
            };

            GitRepositoryInspector inspector{ _gitExecutable, _executor };
            const auto sourcePath = details::DecodeGitWorktreeReplayPath(
                journal.SourceWorktreePath);
            if (!sourcePath)
            {
                return preservedFailure(_failure(
                    GitWorktreeBaselineStatus::InvalidRequest,
                    ERROR_BAD_PATHNAME,
                    "Task Worktree baseline source path is invalid"));
            }
            const auto sourceTime = remaining();
            if (!sourceTime)
            {
                return preservedFailure(_failure(
                    GitWorktreeBaselineStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Task Worktree baseline expired before source inspection"));
            }
            const auto source = inspector.Inspect(
                *sourcePath,
                cancellationEvent,
                (std::min)(*sourceTime, MaximumGitCommandTimeoutMilliseconds));
            if (!source.Succeeded())
            {
                return preservedFailure(_failure(
                    _inspectionStatus(source.Status),
                    source.SystemError,
                    "Task Worktree baseline source inspection failed"));
            }
            if (source.Inspection->CommonDirectoryIdentity.Digest() !=
                    journal.RepositoryIdentityDigest ||
                source.Inspection->ObjectFormat != journal.ObjectFormat ||
                source.Inspection->Head != journal.SourceHead ||
                source.Inspection->BranchRef != journal.SourceReference)
            {
                return preservedFailure(_failure(
                    GitWorktreeBaselineStatus::TargetChanged,
                    ERROR_REVISION_MISMATCH,
                    "Task Worktree baseline source no longer matches its journal"));
            }

            const auto lockTime = remaining();
            if (!lockTime)
            {
                return preservedFailure(_failure(
                    GitWorktreeBaselineStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Task Worktree baseline expired before repository locking"));
            }
            auto gitLock = AcquireGitRepositoryLock(
                source.Inspection->CommonDirectoryIdentity,
                cancellationEvent,
                (std::min)(*lockTime, GitRepositoryLockTimeoutMilliseconds));
            if (!gitLock.Acquired())
            {
                return preservedFailure(_failure(
                    _lockStatus(gitLock.Status),
                    gitLock.SystemError,
                    "Task Worktree baseline common-directory lock failed"));
            }

            const auto targetTime = remaining();
            if (!targetTime)
            {
                return preservedFailure(_failure(
                    GitWorktreeBaselineStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Task Worktree baseline expired before target inspection"));
            }
            const auto target = inspector.Inspect(
                layout.WorktreePath,
                cancellationEvent,
                (std::min)(*targetTime, MaximumGitCommandTimeoutMilliseconds));
            if (!target.Succeeded() ||
                target.Inspection->CommonDirectoryIdentity.Digest() !=
                    journal.RepositoryIdentityDigest ||
                target.Inspection->ObjectFormat != journal.ObjectFormat ||
                target.Inspection->BranchRef != layout.BranchRef)
            {
                return preservedFailure(_failure(
                    target.Succeeded() ?
                        GitWorktreeBaselineStatus::TargetChanged :
                        _inspectionStatus(target.Status),
                    target.SystemError == ERROR_SUCCESS ?
                        ERROR_REVISION_MISMATCH :
                        target.SystemError,
                    "Task Worktree baseline target identity is invalid"));
            }

            if (target.Inspection->Head == journal.SourceHead)
            {
                const auto preflightTime = remaining();
                if (!preflightTime)
                {
                    return preservedFailure(_failure(
                        GitWorktreeBaselineStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Task Worktree baseline expired before repository preflight"));
                }
                GitWorktreePreflightInspector preflight{
                    _gitExecutable,
                    _executor
                };
                const auto supported = preflight.Inspect(
                    layout,
                    marker,
                    journal,
                    cancellationEvent,
                    GitWorktreePreflightMode::AllowOwnedRecovery,
                    (std::min)(
                        *preflightTime,
                        MaximumGitWorktreePreflightTimeoutMilliseconds));
                if (!supported.Succeeded() ||
                    supported.Disposition !=
                        GitWorktreePreflightDisposition::ResumeBranch)
                {
                    return preservedFailure(_failure(
                        supported.Status == GitWorktreePreflightStatus::Cancelled ?
                            GitWorktreeBaselineStatus::Cancelled :
                        supported.Status == GitWorktreePreflightStatus::TimedOut ?
                            GitWorktreeBaselineStatus::TimedOut :
                        supported.Status == GitWorktreePreflightStatus::ResourceFailure ?
                            GitWorktreeBaselineStatus::ResourceFailure :
                            GitWorktreeBaselineStatus::TargetChanged,
                        supported.SystemError,
                        supported.Error.empty() ?
                            "Task Worktree baseline repository preflight failed" :
                            supported.Error));
                }
            }

            GitWorktreeBaselineResult commandFailure;
            const auto run = [&](std::vector<std::wstring> arguments) ->
                std::optional<GitCommandResult> {
                const auto time = remaining();
                if (!time)
                {
                    commandFailure = _failure(
                        GitWorktreeBaselineStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Task Worktree baseline command deadline expired");
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
                request.MaximumStdoutBytes = BaselineStdoutBytes;
                request.MaximumStderrBytes = BaselineStderrBytes;
                auto result = _executor(request);
                if (result.Status != GitCommandStatus::Completed)
                {
                    commandFailure = _failure(
                        _commandStatus(result.Status),
                        result.SystemError,
                        "Task Worktree baseline Git command failed");
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
                return preservedFailure(commandFailure);
            }
            const auto headTree = details::ParseGitSourceLine(*headTreeResult);
            if (headTreeResult->ExitCode != 0 || !headTree ||
                !details::IsGitSourceObjectId(*headTree, journal.ObjectFormat))
            {
                return preservedFailure(_failure(
                    GitWorktreeBaselineStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Task Worktree baseline source tree is invalid"));
            }
            const auto cleanSource = journal.WorktreeTree == *headTree &&
                                     journal.IndexTree == *headTree &&
                                     journal.UntrackedFiles.empty();

            const auto updateMarker = [&](const std::string& baseline,
                                          const bool commitCreated,
                                          const bool fallback,
                                          const bool signing,
                                          const GitWorktreeBaselineStatus successStatus) {
                auto replacement = marker;
                replacement.BaselineCommit = baseline;
                const auto updated = ReplaceGitWorktreeMarker(
                    layout,
                    marker,
                    replacement);
                if (!updated.Succeeded())
                {
                    auto result = _failure(
                        GitWorktreeBaselineStatus::MarkerUpdateFailed,
                        updated.SystemError,
                        "Task Worktree baseline marker could not be advanced");
                    result.BaselineCommit = baseline;
                    result.CommitCreated = commitCreated;
                    result.UsedFallbackIdentity = fallback;
                    result.SigningConfigured = signing;
                    return preservedFailure(std::move(result));
                }
                GitWorktreeBaselineResult result;
                result.Status = successStatus;
                result.BaselineCommit = baseline;
                result.Marker = *updated.Marker;
                result.CommitCreated = commitCreated;
                result.UsedFallbackIdentity = fallback;
                result.SigningConfigured = signing;
                return result;
            };

            if (!marker.BaselineCommit.empty())
            {
                if (target.Inspection->Head != marker.BaselineCommit)
                {
                    return preservedFailure(_failure(
                        GitWorktreeBaselineStatus::TargetChanged,
                        ERROR_REVISION_MISMATCH,
                        "Persisted Task Worktree baseline is not checked out"));
                }
                if (marker.BaselineCommit != journal.SourceHead)
                {
                    const auto verified =
                        details::ValidateGitWorktreeCommittedBaseline(
                            marker.BaselineCommit,
                            marker,
                            journal,
                            invoke,
                            target.Inspection->WorktreeIdentity,
                            cancellationEvent);
                    if (!verified.Succeeded)
                    {
                        return preservedFailure(verified.Failure);
                    }
                }
                else
                {
                    if (!cleanSource)
                    {
                        return preservedFailure(_failure(
                            GitWorktreeBaselineStatus::VerificationFailed,
                            ERROR_REVISION_MISMATCH,
                            "Dirty source snapshot cannot use SourceHead as its baseline"));
                    }
                    const auto proof = details::CaptureGitWorktreeReplayProof(
                        journal.ObjectFormat,
                        invoke);
                    if (!proof.Succeeded() ||
                        !details::GitWorktreeReplayTrackedMatches(
                            *proof.Proof,
                            journal) ||
                        !details::GitWorktreeReplayManifestMatches(
                            *proof.Proof,
                            journal))
                    {
                        return preservedFailure(_failure(
                            GitWorktreeBaselineStatus::VerificationFailed,
                            proof.SystemError == ERROR_SUCCESS ?
                                ERROR_REVISION_MISMATCH :
                                proof.SystemError,
                            "Clean Task Worktree baseline proof is invalid"));
                    }
                }
                GitWorktreeBaselineResult result;
                result.Status = GitWorktreeBaselineStatus::AlreadyCreated;
                result.BaselineCommit = marker.BaselineCommit;
                result.Marker = marker;
                return result;
            }

            if (target.Inspection->Head != journal.SourceHead)
            {
                const auto recovered =
                    details::ValidateGitWorktreeCommittedBaseline(
                        target.Inspection->Head,
                        marker,
                        journal,
                        invoke,
                        target.Inspection->WorktreeIdentity,
                        cancellationEvent);
                if (!recovered.Succeeded)
                {
                    return preservedFailure(recovered.Failure);
                }
                return updateMarker(
                    target.Inspection->Head,
                    false,
                    false,
                    false,
                    GitWorktreeBaselineStatus::AlreadyCreated);
            }

            auto proof = details::CaptureGitWorktreeReplayProof(
                journal.ObjectFormat,
                invoke);
            if (!proof.Succeeded())
            {
                return preservedFailure(
                    commandFailure.Error.empty() ?
                        _failure(
                            GitWorktreeBaselineStatus::VerificationFailed,
                            proof.SystemError,
                            proof.Error) :
                        commandFailure);
            }
            if (cleanSource)
            {
                if (!details::GitWorktreeReplayTrackedMatches(
                        *proof.Proof,
                        journal) ||
                    !details::GitWorktreeReplayManifestMatches(
                        *proof.Proof,
                        journal))
                {
                    return preservedFailure(_failure(
                        GitWorktreeBaselineStatus::TargetChanged,
                        ERROR_REVISION_MISMATCH,
                        "Clean Task Worktree does not match its source snapshot"));
                }
                return updateMarker(
                    journal.SourceHead,
                    false,
                    false,
                    false,
                    GitWorktreeBaselineStatus::Succeeded);
            }

            details::GitWorktreeBaselineTreeValidationResult candidate;
            if (details::GitWorktreeReplayTrackedMatches(*proof.Proof, journal) &&
                details::GitWorktreeReplayManifestMatches(*proof.Proof, journal))
            {
                const auto files = details::VerifyGitWorktreeReplayFiles(
                    target.Inspection->WorktreeIdentity,
                    journal,
                    cancellationEvent);
                if (!files.Succeeded())
                {
                    return preservedFailure(_failure(
                        files.Status == GitWorktreeReplayStatus::Cancelled ?
                            GitWorktreeBaselineStatus::Cancelled :
                        files.Status == GitWorktreeReplayStatus::ResourceFailure ?
                            GitWorktreeBaselineStatus::ResourceFailure :
                            GitWorktreeBaselineStatus::VerificationFailed,
                        files.SystemError,
                        files.Error));
                }
                const auto staged = run({
                    L"-c",
                    L"core.hooksPath=NUL",
                    L"add",
                    L"-A",
                    L"--",
                });
                if (!staged)
                {
                    return preservedFailure(commandFailure);
                }
                if (staged->ExitCode != 0)
                {
                    return preservedFailure(_failure(
                        GitWorktreeBaselineStatus::GitFailure,
                        ERROR_INVALID_DATA,
                        "Git could not stage the Task Worktree baseline"));
                }
                proof = details::CaptureGitWorktreeReplayProof(
                    journal.ObjectFormat,
                    invoke);
                if (!proof.Succeeded())
                {
                    return preservedFailure(
                        commandFailure.Error.empty() ?
                            _failure(
                                GitWorktreeBaselineStatus::VerificationFailed,
                                proof.SystemError,
                                proof.Error) :
                            commandFailure);
                }
            }
            candidate = details::ValidateGitWorktreeStagedBaseline(
                *proof.Proof,
                journal,
                invoke,
                target.Inspection->WorktreeIdentity,
                cancellationEvent);
            if (!candidate.Succeeded)
            {
                return preservedFailure(candidate.Failure);
            }

            const auto nonempty = run({
                L"diff",
                L"--cached",
                L"--quiet",
                L"--exit-code",
                L"HEAD",
                L"--",
            });
            if (!nonempty)
            {
                return preservedFailure(commandFailure);
            }
            if (nonempty->ExitCode != 1)
            {
                return preservedFailure(_failure(
                    GitWorktreeBaselineStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Task Worktree baseline unexpectedly has no staged changes"));
            }

            const auto policy = details::InspectGitWorktreeCommitPolicy(invoke);
            if (!policy.Succeeded)
            {
                return preservedFailure(policy.Failure);
            }
            std::vector<std::wstring> commitArguments{
                L"-c",
                L"core.hooksPath=NUL",
            };
            if (policy.UseFallbackIdentity)
            {
                commitArguments.insert(
                    commitArguments.end(),
                    {
                        L"-c",
                        L"user.name=Richer Task Runtime",
                        L"-c",
                        L"user.email=task-runtime@localhost",
                    });
            }
            commitArguments.insert(
                commitArguments.end(),
                {
                    L"commit",
                    L"--no-verify",
                    L"-m",
                    details::WidenGitSourceAscii(
                        details::GitWorktreeBaselineSubject(marker)),
                });
            const auto committed = run(std::move(commitArguments));
            if (!committed)
            {
                auto result = commandFailure;
                result.UsedFallbackIdentity = policy.UseFallbackIdentity;
                result.SigningConfigured = policy.SigningConfigured;
                return preservedFailure(std::move(result));
            }
            if (committed->ExitCode != 0)
            {
                auto result = _failure(
                    policy.SigningConfigured ?
                        GitWorktreeBaselineStatus::SigningFailure :
                        GitWorktreeBaselineStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    policy.SigningConfigured ?
                        "Configured Git signing failed for the Task baseline" :
                        "Git could not create the Task Worktree baseline commit");
                result.UsedFallbackIdentity = policy.UseFallbackIdentity;
                result.SigningConfigured = policy.SigningConfigured;
                return preservedFailure(std::move(result));
            }

            const auto committedHead = run({
                L"rev-parse",
                L"--verify",
                L"HEAD",
            });
            if (!committedHead)
            {
                return preservedFailure(commandFailure);
            }
            const auto baseline = details::ParseGitSourceLine(*committedHead);
            if (committedHead->ExitCode != 0 || !baseline ||
                !details::IsGitSourceObjectId(*baseline, journal.ObjectFormat) ||
                *baseline == journal.SourceHead)
            {
                return preservedFailure(_failure(
                    GitWorktreeBaselineStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Task Worktree baseline commit ID is invalid"));
            }
            const auto verified = details::ValidateGitWorktreeCommittedBaseline(
                *baseline,
                marker,
                journal,
                invoke,
                target.Inspection->WorktreeIdentity,
                cancellationEvent);
            if (!verified.Succeeded || verified.Tree != candidate.Tree)
            {
                return preservedFailure(
                    verified.Succeeded ?
                        _failure(
                            GitWorktreeBaselineStatus::VerificationFailed,
                            ERROR_CRC,
                            "Task Worktree baseline commit tree changed") :
                        verified.Failure);
            }
            return updateMarker(
                *baseline,
                true,
                policy.UseFallbackIdentity,
                policy.SigningConfigured,
                GitWorktreeBaselineStatus::Succeeded);
        }
        catch (...)
        {
            return _failure(
                GitWorktreeBaselineStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree baseline creation exhausted local resources");
        }
    }
}
