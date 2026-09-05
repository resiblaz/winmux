// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeFinalize.h"

#include "GitSourceCommandOutput.h"
#include "GitWorktreeFinalizeInternal.h"
#include "GitWorktreePreflight.h"
#include "GitWorktreeReplayInternal.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        GitWorktreeFinalizeResult _failure(
            const GitWorktreeFinalizeStatus status,
            const DWORD systemError,
            std::string error,
            const GitWorktreeLayout& layout,
            const bool preserved)
        {
            GitWorktreeFinalizeResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            result.Preserved = preserved;
            result.PreservedPath = layout.WorktreePathUtf8;
            return result;
        }

        GitWorktreeFinalizeResult _contextFailure(
            const details::GitWorktreeFinalizeContextResult& context,
            const GitWorktreeLayout& layout,
            const bool preserved)
        {
            return _failure(
                context.Status,
                context.SystemError,
                context.Error,
                layout,
                preserved);
        }

        GitWorktreeFinalizeResult _commandFailure(
            const details::GitWorktreeFinalizeCommandContext& context,
            const GitWorktreeLayout& layout,
            const bool preserved)
        {
            return _failure(
                context.FailureStatus,
                context.FailureSystemError,
                context.FailureError,
                layout,
                preserved);
        }

        std::optional<details::GitWorktreeFinalizeRegistrationResult>
        _registrations(
            details::GitWorktreeFinalizeCommandContext& context,
            const GitWorktreeLayout& layout,
            GitWorktreeFinalizeResult& failure,
            const bool preserved)
        {
            const auto listed = context.RunSource({
                L"worktree",
                L"list",
                L"--porcelain",
                L"-z",
            });
            if (!listed)
            {
                failure = _commandFailure(context, layout, preserved);
                return std::nullopt;
            }
            const auto parsed = details::ParseGitWorktreeList(
                listed->Stdout,
                context.Source.ObjectFormat);
            if (listed->ExitCode != 0 || !parsed.Succeeded)
            {
                failure = _failure(
                    GitWorktreeFinalizeStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Git Worktree registration list is invalid",
                    layout,
                    preserved);
                return std::nullopt;
            }
            auto found = details::FindGitWorktreeFinalizeRegistration(
                layout,
                parsed.Registrations);
            if (!found.Succeeded)
            {
                failure = _failure(
                    GitWorktreeFinalizeStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    found.Error,
                    layout,
                    preserved);
                return std::nullopt;
            }
            return found;
        }

        std::optional<std::string> _branchHead(
            details::GitWorktreeFinalizeCommandContext& context,
            const GitWorktreeLayout& layout,
            GitWorktreeFinalizeResult& failure,
            const bool preserved,
            bool& exists)
        {
            const auto command = context.RunSource({
                L"rev-parse",
                L"--verify",
                details::WidenGitSourceAscii(layout.BranchRef),
            });
            if (!command)
            {
                failure = _commandFailure(context, layout, preserved);
                return std::nullopt;
            }
            if (command->ExitCode != 0)
            {
                if (!command->Stdout.empty())
                {
                    failure = _failure(
                        GitWorktreeFinalizeStatus::VerificationFailed,
                        ERROR_INVALID_DATA,
                        "Missing Task branch returned unexpected output",
                        layout,
                        preserved);
                    return std::nullopt;
                }
                exists = false;
                return std::string{};
            }
            const auto parsed = details::ParseGitSourceLine(*command);
            if (!parsed || !details::IsGitSourceObjectId(
                               *parsed,
                               context.Source.ObjectFormat))
            {
                failure = _failure(
                    GitWorktreeFinalizeStatus::VerificationFailed,
                    ERROR_INVALID_DATA,
                    "Task branch commit ID is invalid",
                    layout,
                    preserved);
                return std::nullopt;
            }
            exists = true;
            return *parsed;
        }

        bool _baselineAncestor(
            details::GitWorktreeFinalizeCommandContext& context,
            const GitWorktreeLayout& layout,
            const GitWorktreeMarker& marker,
            const std::string_view commit,
            GitWorktreeFinalizeResult& failure,
            const bool preserved)
        {
            const auto ancestor = context.RunSource({
                L"merge-base",
                L"--is-ancestor",
                details::WidenGitSourceAscii(marker.BaselineCommit),
                details::WidenGitSourceAscii(std::string{ commit }),
            });
            if (!ancestor)
            {
                failure = _commandFailure(context, layout, preserved);
                return false;
            }
            if (ancestor->ExitCode != 0)
            {
                failure = _failure(
                    GitWorktreeFinalizeStatus::TargetChanged,
                    ERROR_REVISION_MISMATCH,
                    "Task branch no longer descends from its baseline",
                    layout,
                    preserved);
                return false;
            }
            return true;
        }
    }

    GitWorktreeFinalizer::GitWorktreeFinalizer(
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

    GitWorktreeFinalizeResult GitWorktreeFinalizer::Finalize(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& expectedMarker,
        const std::optional<GitWorktreeSourceJournal>& journal,
        const std::filesystem::path& repositoryRoot,
        const bool expectedUndeliveredWork,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) const noexcept
    {
        try
        {
            if (!_executor || _gitExecutable.empty() || repositoryRoot.empty() ||
                !cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE ||
                timeoutMilliseconds == 0 ||
                timeoutMilliseconds > MaximumGitWorktreeFinalizeTimeoutMilliseconds ||
                !ValidateGitWorktreeMarker(layout, expectedMarker) ||
                expectedMarker.BaselineCommit.empty())
            {
                return _failure(
                    GitWorktreeFinalizeStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Task Worktree finalization request is invalid",
                    layout,
                    false);
            }
            DWORD pathError{};
            const auto pathExists = details::GitWorktreeFinalizePathExists(
                layout.WorktreePath,
                pathError);
            if (!pathExists)
            {
                return _failure(
                    GitWorktreeFinalizeStatus::InspectionFailed,
                    pathError,
                    "Task Worktree path state is unavailable",
                    layout,
                    false);
            }
            auto initiallyPresent = *pathExists;
            if (initiallyPresent)
            {
                if (!journal ||
                    !details::ValidateGitWorktreeReplayEvidence(
                         layout,
                         expectedMarker,
                         *journal)
                         .Succeeded())
                {
                    return _failure(
                        GitWorktreeFinalizeStatus::EvidenceMismatch,
                        ERROR_REVISION_MISMATCH,
                        "Task Worktree finalization ownership evidence does not match",
                        layout,
                        true);
                }
            }
            else
            {
                const auto persistedMarker = ReadGitWorktreeMarker(layout);
                if (persistedMarker.Succeeded())
                {
                    const auto expectedBytes =
                        details::EncodeGitWorktreeMarker(expectedMarker);
                    const auto actualBytes =
                        details::EncodeGitWorktreeMarker(*persistedMarker.Marker);
                    if (!expectedBytes || !actualBytes ||
                        *expectedBytes != *actualBytes)
                    {
                        return _failure(
                            GitWorktreeFinalizeStatus::EvidenceMismatch,
                            ERROR_REVISION_MISMATCH,
                            "Removed Worktree ownership marker changed",
                            layout,
                            false);
                    }
                }
                else if (persistedMarker.Status != GitWorktreeMarkerStatus::Missing)
                {
                    return _failure(
                        GitWorktreeFinalizeStatus::EvidenceMismatch,
                        persistedMarker.SystemError,
                        "Removed Worktree ownership marker is unsafe",
                        layout,
                        false);
                }
            }

            auto opened = details::OpenGitWorktreeFinalizeContext(
                _gitExecutable,
                _executor,
                expectedMarker,
                repositoryRoot,
                cancellationEvent,
                timeoutMilliseconds);
            if (!opened.Context)
            {
                return _contextFailure(opened, layout, initiallyPresent);
            }
            auto& context = *opened.Context;
            GitWorktreeFinalizeResult failure;
            const auto registrations = _registrations(
                context,
                layout,
                failure,
                initiallyPresent);
            if (!registrations)
            {
                return failure;
            }
            if (initiallyPresent && !registrations->Managed && !registrations->BranchRegisteredElsewhere)
            {
                // Git may have removed the registration and files before an
                // occupied root directory made removal fail. Ownership evidence
                // was verified above. Only remove an empty, ordinary directory;
                // never recursively delete an unregistered path on retry.
                const auto attributes = GetFileAttributesW(layout.WorktreePath.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    RemoveDirectoryW(layout.WorktreePath.c_str()))
                {
                    initiallyPresent = false;
                }
            }
            if (registrations->BranchRegisteredElsewhere ||
                (initiallyPresent != registrations->Managed.has_value()))
            {
                return _failure(
                    GitWorktreeFinalizeStatus::EvidenceMismatch,
                    ERROR_REVISION_MISMATCH,
                    "Managed Worktree path and Git registration disagree",
                    layout,
                    initiallyPresent);
            }

            std::string finalCommit;
            std::string checkpointCommit;
            bool checkpointCreated{};
            bool usedFallback{};
            bool signing{};
            bool produced{};
            bool branchRetained{};
            bool branchDeleted{};
            bool worktreeRemoved{ !initiallyPresent };
            if (initiallyPresent)
            {
                const auto& registration = *registrations->Managed;
                if (registration.Bare || registration.Detached ||
                    registration.Locked || registration.Prunable ||
                    registration.BranchRef != layout.BranchRef)
                {
                    return _failure(
                        GitWorktreeFinalizeStatus::EvidenceMismatch,
                        ERROR_REVISION_MISMATCH,
                        "Managed Worktree registration is not removable",
                        layout,
                        true);
                }
                GitRepositoryInspector inspector{ _gitExecutable, _executor };
                const auto target = inspector.Inspect(
                    layout.WorktreePath,
                    cancellationEvent,
                    MaximumGitCommandTimeoutMilliseconds);
                if (!target.Succeeded() ||
                    target.Inspection->CommonDirectoryIdentity.Digest() !=
                        expectedMarker.RepositoryIdentityDigest ||
                    target.Inspection->ObjectFormat != context.Source.ObjectFormat ||
                    target.Inspection->BranchRef != layout.BranchRef ||
                    target.Inspection->DetachedHead ||
                    target.Inspection->Head != registration.Head)
                {
                    return _failure(
                        target.Succeeded() ?
                            GitWorktreeFinalizeStatus::TargetChanged :
                            details::MapGitWorktreeFinalizeInspectionStatus(target.Status),
                        target.SystemError == ERROR_SUCCESS ?
                            ERROR_REVISION_MISMATCH : target.SystemError,
                        "Managed Worktree target identity changed",
                        layout,
                        true);
                }
                const auto checkpoint = details::CheckpointGitWorktreeForFinalization(
                    context,
                    layout,
                    expectedMarker,
                    *target.Inspection);
                if (!checkpoint.Succeeded)
                {
                    return checkpoint.Failure;
                }
                finalCommit = checkpoint.FinalCommit;
                checkpointCommit = checkpoint.CheckpointCommit;
                checkpointCreated = checkpoint.CommitCreated;
                usedFallback = checkpoint.UsedFallbackIdentity;
                signing = checkpoint.SigningConfigured;
                if (!_baselineAncestor(
                        context,
                        layout,
                        expectedMarker,
                        finalCommit,
                        failure,
                        true))
                {
                    return failure;
                }

                bool branchExists{};
                const auto branch = _branchHead(
                    context,
                    layout,
                    failure,
                    true,
                    branchExists);
                if (!branch || !branchExists || *branch != finalCommit)
                {
                    return branch ?
                               _failure(
                                   GitWorktreeFinalizeStatus::EvidenceMismatch,
                                   ERROR_REVISION_MISMATCH,
                                   "Task branch does not carry final Worktree output",
                                   layout,
                                   true) :
                               failure;
                }
                const auto finalRegistrations = _registrations(
                    context,
                    layout,
                    failure,
                    true);
                if (!finalRegistrations || !finalRegistrations->Managed ||
                    finalRegistrations->BranchRegisteredElsewhere ||
                    finalRegistrations->Managed->Head != finalCommit ||
                    finalRegistrations->Managed->BranchRef != layout.BranchRef)
                {
                    return finalRegistrations ?
                               _failure(
                                   GitWorktreeFinalizeStatus::EvidenceMismatch,
                                   ERROR_REVISION_MISMATCH,
                                   "Final Worktree registration proof changed",
                                   layout,
                                   true) :
                               failure;
                }

                // Detect cwd/other non-delete-sharing handles before Git can
                // partially remove the worktree and its registration.
                const auto deleteProbe = CreateFileW(
                    layout.WorktreePath.c_str(), DELETE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (deleteProbe == INVALID_HANDLE_VALUE)
                {
                    return _failure(GitWorktreeFinalizeStatus::GitFailure, GetLastError(),
                                    "Close programs using the Task Worktree directory and retry cleanup",
                                    layout, true);
                }
                CloseHandle(deleteProbe);
                const auto removed = context.RunSource({
                    L"-c",
                    L"core.hooksPath=NUL",
                    L"worktree",
                    L"remove",
                    L"--",
                    layout.WorktreePath.native(),
                });
                if (!removed)
                {
                    return _commandFailure(context, layout, true);
                }
                if (removed->ExitCode != 0)
                {
                    return _failure(
                        GitWorktreeFinalizeStatus::GitFailure,
                        ERROR_DIR_NOT_EMPTY,
                        "Git could not remove the clean managed Worktree",
                        layout,
                        true);
                }
                worktreeRemoved = true;
                const auto absent = details::GitWorktreeFinalizePathExists(
                    layout.WorktreePath,
                    pathError);
                const auto after = _registrations(
                    context,
                    layout,
                    failure,
                    false);
                if (!absent || *absent || !after || after->Managed ||
                    after->BranchRegisteredElsewhere)
                {
                    return after ?
                               _failure(
                                   GitWorktreeFinalizeStatus::VerificationFailed,
                                   pathError == ERROR_SUCCESS ?
                                       ERROR_DIR_NOT_EMPTY : pathError,
                                   "Managed Worktree removal proof is incomplete",
                                   layout,
                                   false) :
                               failure;
                }
                produced = finalCommit != expectedMarker.BaselineCommit;
            }
            else
            {
                bool branchExists{};
                const auto branch = _branchHead(
                    context,
                    layout,
                    failure,
                    false,
                    branchExists);
                if (!branch)
                {
                    return failure;
                }
                if (!branchExists)
                {
                    if (expectedUndeliveredWork)
                    {
                        return _failure(
                            GitWorktreeFinalizeStatus::EvidenceMismatch,
                            ERROR_REVISION_MISMATCH,
                            "Delivered result branch is unexpectedly missing",
                            layout,
                            false);
                    }
                    finalCommit = expectedMarker.BaselineCommit;
                }
                else
                {
                    finalCommit = *branch;
                    if (!_baselineAncestor(
                            context,
                            layout,
                            expectedMarker,
                            finalCommit,
                            failure,
                            false))
                    {
                        return failure;
                    }
                    produced = finalCommit != expectedMarker.BaselineCommit;
                }
            }

            if (produced)
            {
                bool branchExists{};
                const auto branch = _branchHead(
                    context,
                    layout,
                    failure,
                    false,
                    branchExists);
                if (!branch || !branchExists || *branch != finalCommit)
                {
                    return branch ?
                               _failure(
                                   GitWorktreeFinalizeStatus::EvidenceMismatch,
                                   ERROR_REVISION_MISMATCH,
                                   "Result branch changed after Worktree removal",
                                   layout,
                                   false) :
                               failure;
                }
                branchRetained = true;
            }
            else
            {
                bool branchExists{};
                const auto branch = _branchHead(
                    context,
                    layout,
                    failure,
                    false,
                    branchExists);
                if (!branch)
                {
                    return failure;
                }
                if (branchExists)
                {
                    if (*branch != expectedMarker.BaselineCommit)
                    {
                        return _failure(
                            GitWorktreeFinalizeStatus::EvidenceMismatch,
                            ERROR_REVISION_MISMATCH,
                            "Baseline-equal Task branch changed before deletion",
                            layout,
                            false);
                    }
                    const auto deleted = context.RunSource({
                        L"update-ref",
                        L"-d",
                        details::WidenGitSourceAscii(layout.BranchRef),
                        details::WidenGitSourceAscii(expectedMarker.BaselineCommit),
                    });
                    if (!deleted)
                    {
                        return _commandFailure(context, layout, false);
                    }
                    if (deleted->ExitCode != 0)
                    {
                        return _failure(
                            GitWorktreeFinalizeStatus::GitFailure,
                            ERROR_REVISION_MISMATCH,
                            "Git could not CAS-delete the baseline-equal Task branch",
                            layout,
                            false);
                    }
                }
                bool remaining{};
                const auto proof = _branchHead(
                    context,
                    layout,
                    failure,
                    false,
                    remaining);
                if (!proof || remaining)
                {
                    return proof ?
                               _failure(
                                   GitWorktreeFinalizeStatus::VerificationFailed,
                                   ERROR_REVISION_MISMATCH,
                                   "Baseline-equal Task branch still exists",
                                   layout,
                                   false) :
                               failure;
                }
                branchDeleted = true;
            }

            const auto sidecars = details::CleanupGitWorktreeFinalizeSidecars(
                layout,
                expectedMarker);
            if (!sidecars.SidecarsRemoved)
            {
                auto result = sidecars;
                result.FinalCommit = finalCommit;
                result.CheckpointCommit = checkpointCommit;
                result.CheckpointCreated = checkpointCreated;
                result.UsedFallbackIdentity = usedFallback;
                result.SigningConfigured = signing;
                result.ProducedWork = produced;
                result.BranchRetained = branchRetained;
                result.BranchDeleted = branchDeleted;
                result.WorktreeRemoved = worktreeRemoved;
                result.PreservedPath = layout.WorktreePathUtf8;
                return result;
            }

            GitWorktreeFinalizeResult result;
            result.Status = initiallyPresent ?
                                GitWorktreeFinalizeStatus::Succeeded :
                                GitWorktreeFinalizeStatus::AlreadyFinalized;
            result.FinalCommit = std::move(finalCommit);
            result.CheckpointCommit = std::move(checkpointCommit);
            result.CheckpointCreated = checkpointCreated;
            result.UsedFallbackIdentity = usedFallback;
            result.SigningConfigured = signing;
            result.ProducedWork = produced;
            result.BranchRetained = branchRetained;
            result.WorktreeRemoved = worktreeRemoved;
            result.BranchDeleted = branchDeleted;
            result.SidecarsRemoved = true;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeFinalizeStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree finalization exhausted local resources",
                layout,
                false);
        }
    }
}
