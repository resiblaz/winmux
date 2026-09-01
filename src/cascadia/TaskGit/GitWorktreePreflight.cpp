// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreePreflight.h"

#include "GitSourceCapturePolicy.h"
#include "GitWorktreePreflightInternal.h"

#include "GitCommandRunner.h"
#include "GitRepositoryLock.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        constexpr size_t PreflightStdoutBytes{
            MaximumGitOutputBytes - 64 * 1024
        };
        constexpr size_t PreflightStderrBytes{ 64 * 1024 };

        GitWorktreePreflightResult _failure(
            const GitWorktreePreflightStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreePreflightResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        GitWorktreePreflightStatus _inspectionStatus(
            const GitRepositoryInspectionStatus status) noexcept
        {
            switch (status)
            {
            case GitRepositoryInspectionStatus::Cancelled:
                return GitWorktreePreflightStatus::Cancelled;
            case GitRepositoryInspectionStatus::TimedOut:
                return GitWorktreePreflightStatus::TimedOut;
            case GitRepositoryInspectionStatus::LockFailed:
                return GitWorktreePreflightStatus::LockFailed;
            case GitRepositoryInspectionStatus::InvalidOutput:
                return GitWorktreePreflightStatus::InvalidGitOutput;
            case GitRepositoryInspectionStatus::ResourceFailure:
                return GitWorktreePreflightStatus::ResourceFailure;
            default:
                return GitWorktreePreflightStatus::InspectionFailed;
            }
        }

        GitWorktreePreflightStatus _commandStatus(
            const GitCommandStatus status) noexcept
        {
            switch (status)
            {
            case GitCommandStatus::Cancelled:
                return GitWorktreePreflightStatus::Cancelled;
            case GitCommandStatus::TimedOut:
                return GitWorktreePreflightStatus::TimedOut;
            case GitCommandStatus::OutputLimitExceeded:
            case GitCommandStatus::InvalidUtf8:
                return GitWorktreePreflightStatus::InvalidGitOutput;
            case GitCommandStatus::InvalidRequest:
                return GitWorktreePreflightStatus::InvalidRequest;
            case GitCommandStatus::ResourceFailure:
                return GitWorktreePreflightStatus::ResourceFailure;
            default:
                return GitWorktreePreflightStatus::GitFailure;
            }
        }

        GitWorktreePreflightStatus _lockStatus(
            const TaskDirectoryLockStatus status) noexcept
        {
            switch (status)
            {
            case TaskDirectoryLockStatus::Cancelled:
                return GitWorktreePreflightStatus::Cancelled;
            case TaskDirectoryLockStatus::TimedOut:
                return GitWorktreePreflightStatus::TimedOut;
            case TaskDirectoryLockStatus::ResourceFailure:
                return GitWorktreePreflightStatus::ResourceFailure;
            default:
                return GitWorktreePreflightStatus::LockFailed;
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
            std::wstring result(static_cast<size_t>(required), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    required) != required)
            {
                return std::nullopt;
            }
            const std::filesystem::path path{ result };
            return path.is_absolute() && path.has_root_path() ?
                       std::optional<std::filesystem::path>{ path.lexically_normal() } :
                       std::nullopt;
        }

        bool _samePath(
            const std::filesystem::path& left,
            const std::filesystem::path& right) noexcept
        {
            try
            {
                return left.native().size() <= static_cast<size_t>(INT_MAX) &&
                       right.native().size() <= static_cast<size_t>(INT_MAX) &&
                       CompareStringOrdinal(
                           left.native().data(),
                           static_cast<int>(left.native().size()),
                           right.native().data(),
                           static_cast<int>(right.native().size()),
                           TRUE) == CSTR_EQUAL;
            }
            catch (...)
            {
                return false;
            }
        }

        bool _matchesJournal(
            const GitRepositoryInspection& inspection,
            const GitWorktreeSourceJournal& journal) noexcept
        {
            try
            {
                const auto sourcePath = _widePath(journal.SourceWorktreePath);
                return sourcePath &&
                       _samePath(
                           inspection.WorktreeIdentity.CanonicalPath(),
                           *sourcePath) &&
                       inspection.CommonDirectoryIdentity.Digest() ==
                           journal.RepositoryIdentityDigest &&
                       inspection.ObjectFormat == journal.ObjectFormat &&
                       inspection.Head == journal.SourceHead &&
                       inspection.BranchRef == journal.SourceReference;
            }
            catch (...)
            {
                return false;
            }
        }

    }

    GitWorktreePreflightInspector::GitWorktreePreflightInspector(
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

    GitWorktreePreflightResult GitWorktreePreflightInspector::Inspect(
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
                    MaximumGitWorktreePreflightTimeoutMilliseconds ||
                (mode != GitWorktreePreflightMode::RequireUnoccupied &&
                 mode != GitWorktreePreflightMode::AllowOwnedRecovery) ||
                !ValidateGitWorktreeSourceJournal(layout, marker, journal))
            {
                return _failure(
                    GitWorktreePreflightStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree preflight request is invalid");
            }
            const auto sourcePath = _widePath(journal.SourceWorktreePath);
            if (!sourcePath)
            {
                return _failure(
                    GitWorktreePreflightStatus::InvalidRequest,
                    ERROR_BAD_PATHNAME,
                    "Git Worktree preflight source path is invalid");
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
            const auto initialTime = remaining();
            if (!initialTime)
            {
                return _failure(
                    GitWorktreePreflightStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git Worktree preflight expired before repository inspection");
            }
            const auto initial = inspector.Inspect(
                *sourcePath,
                cancellationEvent,
                (std::min)(*initialTime, MaximumGitCommandTimeoutMilliseconds));
            if (!initial.Succeeded())
            {
                return _failure(
                    _inspectionStatus(initial.Status),
                    initial.SystemError,
                    "Git Worktree preflight repository inspection failed");
            }
            if (!_matchesJournal(*initial.Inspection, journal))
            {
                return _failure(
                    GitWorktreePreflightStatus::RepositoryChanged,
                    ERROR_REVISION_MISMATCH,
                    "Git source repository identity or HEAD changed after capture");
            }

            const auto lockTime = remaining();
            if (!lockTime)
            {
                return _failure(
                    GitWorktreePreflightStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git Worktree preflight expired before repository locking");
            }
            auto gitLock = AcquireGitRepositoryLock(
                initial.Inspection->CommonDirectoryIdentity,
                cancellationEvent,
                (std::min)(*lockTime, GitRepositoryLockTimeoutMilliseconds));
            if (!gitLock.Acquired())
            {
                return _failure(
                    _lockStatus(gitLock.Status),
                    gitLock.SystemError,
                    "Git Worktree preflight common-directory lock failed");
            }

            const auto lockedTime = remaining();
            if (!lockedTime)
            {
                return _failure(
                    GitWorktreePreflightStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git Worktree preflight expired after repository locking");
            }
            const auto locked = inspector.Inspect(
                *sourcePath,
                cancellationEvent,
                (std::min)(*lockedTime, MaximumGitCommandTimeoutMilliseconds));
            if (!locked.Succeeded())
            {
                return _failure(
                    _inspectionStatus(locked.Status),
                    locked.SystemError,
                    "Git Worktree preflight locked inspection failed");
            }
            if (!details::SameGitSourceRepository(
                    *initial.Inspection,
                    *locked.Inspection) ||
                !_matchesJournal(*locked.Inspection, journal))
            {
                return _failure(
                    GitWorktreePreflightStatus::RepositoryChanged,
                    ERROR_REVISION_MISMATCH,
                    "Git source repository changed while entering preflight");
            }

            const details::GitWorktreePreflightCommandInvoker invoke =
                [&](std::vector<std::wstring> arguments) {
                details::GitWorktreePreflightCommandInvocation invocation;
                const auto time = remaining();
                if (!time)
                {
                    invocation.Failure = _failure(
                        GitWorktreePreflightStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Git Worktree preflight command deadline expired");
                    return invocation;
                }
                GitCommandRequest request;
                request.GitExecutable = locked.Inspection->GitExecutable;
                request.WorkingDirectory = locked.Inspection->WorktreeIdentity;
                request.Arguments = std::move(arguments);
                request.CancellationEvent = cancellationEvent;
                request.TimeoutMilliseconds = (std::min)(
                    *time,
                    MaximumGitCommandTimeoutMilliseconds);
                request.MaximumStdoutBytes = PreflightStdoutBytes;
                request.MaximumStderrBytes = PreflightStderrBytes;
                auto result = _executor(request);
                if (result.Status != GitCommandStatus::Completed)
                {
                    invocation.Failure = _failure(
                        _commandStatus(result.Status),
                        result.SystemError,
                        "Git Worktree preflight command failed");
                    return invocation;
                }
                invocation.Result = std::move(result);
                return invocation;
            };
            if (const auto unsupported =
                    details::CheckGitWorktreeRepositorySupport(journal, invoke))
            {
                return *unsupported;
            }
            return details::InspectGitWorktreeCollision(
                layout,
                journal,
                mode,
                invoke);
        }
        catch (...)
        {
            return _failure(
                GitWorktreePreflightStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree preflight exhausted local resources");
        }
    }
}
