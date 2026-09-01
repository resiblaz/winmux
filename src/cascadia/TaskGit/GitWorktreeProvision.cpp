// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeProvision.h"

#include "GitSourceCommandOutput.h"
#include "GitWorktreePreflightInternal.h"

#include "GitCommandRunner.h"
#include "GitRepositoryLock.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        constexpr size_t ProvisionStdoutBytes{ 1024 * 1024 };
        constexpr size_t ProvisionStderrBytes{ 1024 * 1024 };

        GitWorktreeProvisionResult _failure(
            const GitWorktreeProvisionStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreeProvisionResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        GitWorktreeProvisionStatus _inspectionStatus(
            const GitRepositoryInspectionStatus status) noexcept
        {
            switch (status)
            {
            case GitRepositoryInspectionStatus::Cancelled:
                return GitWorktreeProvisionStatus::Cancelled;
            case GitRepositoryInspectionStatus::TimedOut:
                return GitWorktreeProvisionStatus::TimedOut;
            case GitRepositoryInspectionStatus::LockFailed:
                return GitWorktreeProvisionStatus::LockFailed;
            case GitRepositoryInspectionStatus::ResourceFailure:
                return GitWorktreeProvisionStatus::ResourceFailure;
            default:
                return GitWorktreeProvisionStatus::InspectionFailed;
            }
        }

        GitWorktreeProvisionStatus _lockStatus(
            const TaskDirectoryLockStatus status) noexcept
        {
            switch (status)
            {
            case TaskDirectoryLockStatus::Cancelled:
                return GitWorktreeProvisionStatus::Cancelled;
            case TaskDirectoryLockStatus::TimedOut:
                return GitWorktreeProvisionStatus::TimedOut;
            case TaskDirectoryLockStatus::ResourceFailure:
                return GitWorktreeProvisionStatus::ResourceFailure;
            default:
                return GitWorktreeProvisionStatus::LockFailed;
            }
        }

        GitWorktreeProvisionStatus _commandStatus(
            const GitCommandStatus status) noexcept
        {
            switch (status)
            {
            case GitCommandStatus::Cancelled:
                return GitWorktreeProvisionStatus::Cancelled;
            case GitCommandStatus::TimedOut:
                return GitWorktreeProvisionStatus::TimedOut;
            case GitCommandStatus::ResourceFailure:
                return GitWorktreeProvisionStatus::ResourceFailure;
            case GitCommandStatus::InvalidRequest:
                return GitWorktreeProvisionStatus::InvalidRequest;
            default:
                return GitWorktreeProvisionStatus::GitFailure;
            }
        }

        GitWorktreeProvisionStatus _verificationStatus(
            const GitWorktreePreflightStatus status) noexcept
        {
            switch (status)
            {
            case GitWorktreePreflightStatus::Cancelled:
                return GitWorktreeProvisionStatus::Cancelled;
            case GitWorktreePreflightStatus::TimedOut:
                return GitWorktreeProvisionStatus::TimedOut;
            case GitWorktreePreflightStatus::ResourceFailure:
                return GitWorktreeProvisionStatus::ResourceFailure;
            default:
                return GitWorktreeProvisionStatus::VerificationFailed;
            }
        }

        std::optional<std::filesystem::path> _widePath(
            const std::string_view value)
        {
            if (value.empty() ||
                value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return std::nullopt;
            }
            const auto required = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0);
            if (required <= 0)
            {
                return std::nullopt;
            }
            std::wstring decoded(static_cast<size_t>(required), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    decoded.data(),
                    required) != required)
            {
                return std::nullopt;
            }
            const std::filesystem::path path{ decoded };
            return path.is_absolute() && path.has_root_path() ?
                       std::optional<std::filesystem::path>{ path.lexically_normal() } :
                       std::nullopt;
        }

        GitWorktreeProvisionResult _preserved(
            GitWorktreeProvisionResult result,
            const GitWorktreeLayout& layout)
        {
            result.Preserved = true;
            result.PreservedPath = layout.WorktreePathUtf8;
            return result;
        }

        GitWorktreeProvisionResult _verificationFailure(
            const GitWorktreePreflightResult& verification,
            const GitWorktreeLayout& layout,
            const GitWorktreePreflightDisposition initialDisposition,
            const bool worktreeCreated,
            const bool branchCreated,
            std::string error)
        {
            const auto effectiveStatus =
                verification.Status == GitWorktreePreflightStatus::Succeeded ?
                    GitWorktreePreflightStatus::InvalidGitOutput :
                    verification.Status;
            auto result = _failure(
                _verificationStatus(effectiveStatus),
                verification.SystemError == ERROR_SUCCESS ?
                    ERROR_INVALID_DATA :
                    verification.SystemError,
                std::move(error));
            result.PreflightStatus = effectiveStatus;
            result.InitialDisposition = initialDisposition;
            result.WorktreeCreated = worktreeCreated;
            result.BranchCreated = branchCreated;
            return _preserved(std::move(result), layout);
        }
    }

    GitWorktreeProvisioner::GitWorktreeProvisioner(
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

    GitWorktreeProvisionResult GitWorktreeProvisioner::Provision(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal,
        const HANDLE cancellationEvent,
        const GitWorktreePreflightMode mode,
        const uint32_t timeoutMilliseconds) const noexcept
    {
        try
        {
            if (!_executor || _gitExecutable.empty() || !cancellationEvent ||
                cancellationEvent == INVALID_HANDLE_VALUE ||
                timeoutMilliseconds == 0 ||
                timeoutMilliseconds >
                    MaximumGitWorktreeProvisionTimeoutMilliseconds ||
                (mode != GitWorktreePreflightMode::RequireUnoccupied &&
                 mode != GitWorktreePreflightMode::AllowOwnedRecovery) ||
                !ValidateGitWorktreeSourceJournal(layout, marker, journal))
            {
                return _failure(
                    GitWorktreeProvisionStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree provision request is invalid");
            }
            const auto sourcePath = _widePath(journal.SourceWorktreePath);
            if (!sourcePath)
            {
                return _failure(
                    GitWorktreeProvisionStatus::InvalidRequest,
                    ERROR_BAD_PATHNAME,
                    "Git Worktree provision source path is invalid");
            }

            const auto persistedMarker = ReadGitWorktreeMarker(layout);
            if (!persistedMarker.Succeeded())
            {
                return _failure(
                    persistedMarker.Status ==
                            GitWorktreeMarkerStatus::ResourceFailure ?
                        GitWorktreeProvisionStatus::ResourceFailure :
                        GitWorktreeProvisionStatus::PreflightRejected,
                    persistedMarker.SystemError,
                    "Git Worktree provision ownership marker is unavailable");
            }
            const auto persistedJournal = ReadGitWorktreeSourceJournal(
                layout,
                *persistedMarker.Marker);
            const auto expectedMarkerBytes = details::EncodeGitWorktreeMarker(
                marker);
            const auto actualMarkerBytes = details::EncodeGitWorktreeMarker(
                *persistedMarker.Marker);
            const auto expectedJournalBytes =
                details::EncodeGitWorktreeSourceJournal(journal);
            const auto actualJournalBytes =
                persistedJournal.Succeeded() ?
                    details::EncodeGitWorktreeSourceJournal(
                        *persistedJournal.Journal) :
                    std::optional<std::vector<uint8_t>>{};
            if (!persistedJournal.Succeeded() || !expectedMarkerBytes ||
                !actualMarkerBytes || !expectedJournalBytes ||
                !actualJournalBytes ||
                *expectedMarkerBytes != *actualMarkerBytes ||
                *expectedJournalBytes != *actualJournalBytes)
            {
                return _failure(
                    persistedJournal.Status ==
                            GitWorktreeSourceJournalStatus::ResourceFailure ?
                        GitWorktreeProvisionStatus::ResourceFailure :
                        GitWorktreeProvisionStatus::PreflightRejected,
                    persistedJournal.SystemError == ERROR_SUCCESS ?
                        ERROR_REVISION_MISMATCH :
                        persistedJournal.SystemError,
                    "Git Worktree provision durable ownership evidence does not match");
            }

            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            const auto remaining = [&]() -> std::optional<uint32_t> {
                const auto now = GetTickCount64();
                return now < deadline ?
                           std::optional<uint32_t>{ static_cast<uint32_t>(
                               (std::min<uint64_t>)(deadline - now, UINT32_MAX)) } :
                           std::nullopt;
            };

            GitRepositoryInspector repositoryInspector{
                _gitExecutable,
                _executor
            };
            const auto inspectionTime = remaining();
            if (!inspectionTime)
            {
                return _failure(
                    GitWorktreeProvisionStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git Worktree provision expired before repository inspection");
            }
            const auto repository = repositoryInspector.Inspect(
                *sourcePath,
                cancellationEvent,
                (std::min)(
                    *inspectionTime,
                    MaximumGitCommandTimeoutMilliseconds));
            if (!repository.Succeeded())
            {
                return _failure(
                    _inspectionStatus(repository.Status),
                    repository.SystemError,
                    "Git Worktree provision repository inspection failed");
            }
            if (repository.Inspection->CommonDirectoryIdentity.Digest() !=
                    journal.RepositoryIdentityDigest ||
                repository.Inspection->ObjectFormat != journal.ObjectFormat ||
                repository.Inspection->Head != journal.SourceHead ||
                repository.Inspection->BranchRef != journal.SourceReference)
            {
                auto result = _failure(
                    GitWorktreeProvisionStatus::PreflightRejected,
                    ERROR_REVISION_MISMATCH,
                    "Git source repository no longer matches its captured journal");
                result.PreflightStatus =
                    GitWorktreePreflightStatus::RepositoryChanged;
                return result;
            }

            const auto lockTime = remaining();
            if (!lockTime)
            {
                return _failure(
                    GitWorktreeProvisionStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git Worktree provision expired before repository locking");
            }
            auto gitLock = AcquireGitRepositoryLock(
                repository.Inspection->CommonDirectoryIdentity,
                cancellationEvent,
                (std::min)(*lockTime, GitRepositoryLockTimeoutMilliseconds));
            if (!gitLock.Acquired())
            {
                return _failure(
                    _lockStatus(gitLock.Status),
                    gitLock.SystemError,
                    "Git Worktree provision common-directory lock failed");
            }

            const auto preflightTime = remaining();
            if (!preflightTime)
            {
                return _failure(
                    GitWorktreeProvisionStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git Worktree provision expired before preflight");
            }
            GitWorktreePreflightInspector preflight{
                _gitExecutable,
                _executor
            };
            const auto inspected = preflight.Inspect(
                layout,
                marker,
                journal,
                cancellationEvent,
                mode,
                (std::min)(
                    *preflightTime,
                    MaximumGitWorktreePreflightTimeoutMilliseconds));
            if (!inspected.Succeeded())
            {
                auto result = _failure(
                    _verificationStatus(inspected.Status) ==
                            GitWorktreeProvisionStatus::VerificationFailed ?
                        GitWorktreeProvisionStatus::PreflightRejected :
                        _verificationStatus(inspected.Status),
                    inspected.SystemError,
                    inspected.Error.empty() ?
                        "Git Worktree provision preflight rejected the request" :
                        inspected.Error);
                result.PreflightStatus = inspected.Status;
                return result;
            }

            const auto initialDisposition = inspected.Disposition;
            GitWorktreeProvisionResult commandFailure;
            const auto run = [&](const CanonicalDirectoryIdentity& directory,
                                 std::vector<std::wstring> arguments) ->
                std::optional<GitCommandResult> {
                const auto time = remaining();
                if (!time)
                {
                    commandFailure = _failure(
                        GitWorktreeProvisionStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Git Worktree provision command deadline expired");
                    return std::nullopt;
                }
                GitCommandRequest request;
                request.GitExecutable = repository.Inspection->GitExecutable;
                request.WorkingDirectory = directory;
                request.Arguments = std::move(arguments);
                request.CancellationEvent = cancellationEvent;
                request.TimeoutMilliseconds = (std::min)(
                    *time,
                    MaximumGitCommandTimeoutMilliseconds);
                request.MaximumStdoutBytes = ProvisionStdoutBytes;
                request.MaximumStderrBytes = ProvisionStderrBytes;
                auto result = _executor(request);
                if (result.Status != GitCommandStatus::Completed)
                {
                    commandFailure = _failure(
                        _commandStatus(result.Status),
                        result.SystemError,
                        "Git Worktree provision command failed");
                    return std::nullopt;
                }
                return result;
            };

            const details::GitWorktreePreflightCommandInvoker inspectCollision =
                [&](std::vector<std::wstring> arguments) {
                details::GitWorktreePreflightCommandInvocation invocation;
                const auto command = run(
                    repository.Inspection->WorktreeIdentity,
                    std::move(arguments));
                if (!command)
                {
                    invocation.Failure.Status =
                        commandFailure.Status == GitWorktreeProvisionStatus::Cancelled ?
                            GitWorktreePreflightStatus::Cancelled :
                        commandFailure.Status == GitWorktreeProvisionStatus::TimedOut ?
                            GitWorktreePreflightStatus::TimedOut :
                        commandFailure.Status == GitWorktreeProvisionStatus::ResourceFailure ?
                            GitWorktreePreflightStatus::ResourceFailure :
                            GitWorktreePreflightStatus::GitFailure;
                    invocation.Failure.SystemError = commandFailure.SystemError;
                    invocation.Failure.Error = commandFailure.Error;
                    return invocation;
                }
                invocation.Result = *command;
                return invocation;
            };

            bool worktreeCreated{};
            bool branchCreated{};
            auto disposition = initialDisposition;
            if (disposition == GitWorktreePreflightDisposition::CreateNew)
            {
                const auto added = run(
                    repository.Inspection->WorktreeIdentity,
                    {
                        L"-c",
                        L"core.hooksPath=NUL",
                        L"worktree",
                        L"add",
                        L"--detach",
                        layout.WorktreePath.native(),
                        details::WidenGitSourceAscii(journal.SourceHead),
                    });
                if (!added)
                {
                    commandFailure.InitialDisposition = initialDisposition;
                    return _preserved(std::move(commandFailure), layout);
                }
                if (added->ExitCode != 0)
                {
                    auto result = _failure(
                        GitWorktreeProvisionStatus::GitFailure,
                        ERROR_INVALID_DATA,
                        "Git could not create the detached Task Worktree");
                    result.InitialDisposition = initialDisposition;
                    return _preserved(std::move(result), layout);
                }
                worktreeCreated = true;
                const auto verified = details::InspectGitWorktreeCollision(
                    layout,
                    journal,
                    GitWorktreePreflightMode::AllowOwnedRecovery,
                    inspectCollision);
                if (!verified.Succeeded() ||
                    verified.Disposition !=
                        GitWorktreePreflightDisposition::ResumeDetached)
                {
                    return _verificationFailure(
                        verified,
                        layout,
                        initialDisposition,
                        worktreeCreated,
                        branchCreated,
                        "Created Task Worktree detached-state verification failed");
                }
                disposition = verified.Disposition;
            }

            if (disposition == GitWorktreePreflightDisposition::ResumeDetached)
            {
                const auto worktree = ResolveCanonicalDirectoryIdentity(
                    layout.WorktreePath,
                    DirectoryIdentityPolicy::Worktree);
                if (!worktree.Succeeded())
                {
                    auto result = _failure(
                        GitWorktreeProvisionStatus::VerificationFailed,
                        worktree.SystemError,
                        "Detached Task Worktree identity is unsafe");
                    result.InitialDisposition = initialDisposition;
                    result.WorktreeCreated = worktreeCreated;
                    return _preserved(std::move(result), layout);
                }
                const auto switched = run(
                    *worktree.Identity,
                    {
                        L"-c",
                        L"core.hooksPath=NUL",
                        L"switch",
                        L"-c",
                        details::WidenGitSourceAscii(layout.BranchName),
                    });
                if (!switched)
                {
                    commandFailure.InitialDisposition = initialDisposition;
                    commandFailure.WorktreeCreated = worktreeCreated;
                    return _preserved(std::move(commandFailure), layout);
                }
                if (switched->ExitCode != 0)
                {
                    auto result = _failure(
                        GitWorktreeProvisionStatus::GitFailure,
                        ERROR_INVALID_DATA,
                        "Git could not attach the deterministic Task branch");
                    result.InitialDisposition = initialDisposition;
                    result.WorktreeCreated = worktreeCreated;
                    return _preserved(std::move(result), layout);
                }
                branchCreated = true;
                const auto verified = details::InspectGitWorktreeCollision(
                    layout,
                    journal,
                    GitWorktreePreflightMode::AllowOwnedRecovery,
                    inspectCollision);
                if (!verified.Succeeded() ||
                    verified.Disposition !=
                        GitWorktreePreflightDisposition::ResumeBranch)
                {
                    return _verificationFailure(
                        verified,
                        layout,
                        initialDisposition,
                        worktreeCreated,
                        branchCreated,
                        "Task Worktree branch verification failed");
                }
                disposition = verified.Disposition;
            }

            if (disposition != GitWorktreePreflightDisposition::ResumeBranch)
            {
                GitWorktreePreflightResult invalid;
                invalid.Status = GitWorktreePreflightStatus::InvalidGitOutput;
                invalid.SystemError = ERROR_INVALID_DATA;
                return _verificationFailure(
                    invalid,
                    layout,
                    initialDisposition,
                    worktreeCreated,
                    branchCreated,
                    "Task Worktree provision reached an invalid disposition");
            }

            GitWorktreeProvisionResult result;
            result.Status = GitWorktreeProvisionStatus::Succeeded;
            result.InitialDisposition = initialDisposition;
            result.WorktreeCreated = worktreeCreated;
            result.BranchCreated = branchCreated;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeProvisionStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree provision exhausted local resources");
        }
    }
}
