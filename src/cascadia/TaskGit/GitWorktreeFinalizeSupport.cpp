// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeFinalizeInternal.h"

#include "GitSourceCommandOutput.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        std::optional<uint32_t> _remaining(const uint64_t deadline) noexcept
        {
            const auto now = GetTickCount64();
            return now < deadline ?
                       std::optional<uint32_t>{ static_cast<uint32_t>(
                           (std::min<uint64_t>)(deadline - now, UINT32_MAX)) } :
                       std::nullopt;
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

        std::optional<bool> _samePath(
            const std::string_view left,
            const std::string_view right)
        {
            const auto leftPath = _widePath(left);
            const auto rightPath = _widePath(right);
            if (!leftPath || !rightPath ||
                leftPath->native().size() > static_cast<size_t>(INT_MAX) ||
                rightPath->native().size() > static_cast<size_t>(INT_MAX))
            {
                return std::nullopt;
            }
            return CompareStringOrdinal(
                       leftPath->native().data(),
                       static_cast<int>(leftPath->native().size()),
                       rightPath->native().data(),
                       static_cast<int>(rightPath->native().size()),
                       TRUE) == CSTR_EQUAL;
        }

        GitWorktreeFinalizeResult _sidecarFailure(
            const DWORD systemError,
            std::string error)
        {
            GitWorktreeFinalizeResult result;
            result.Status = GitWorktreeFinalizeStatus::SidecarCleanupFailed;
            result.SystemError = systemError;
            result.Error = std::move(error);
            result.WorktreeRemoved = true;
            return result;
        }

        std::optional<bool> _fileExists(
            const std::filesystem::path& path,
            DWORD& systemError) noexcept
        {
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                return true;
            }
            systemError = GetLastError();
            if (systemError == ERROR_FILE_NOT_FOUND ||
                systemError == ERROR_PATH_NOT_FOUND)
            {
                systemError = ERROR_SUCCESS;
                return false;
            }
            return std::nullopt;
        }

        bool _deleteRegularSidecar(
            const std::filesystem::path& path,
            DWORD& systemError,
            std::string& error) noexcept
        {
            const auto exists = _fileExists(path, systemError);
            if (!exists)
            {
                error = "Task Worktree sidecar attributes are unavailable";
                return false;
            }
            if (!*exists)
            {
                return true;
            }
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                               FILE_ATTRIBUTE_REPARSE_POINT |
                               FILE_ATTRIBUTE_DEVICE)) != 0)
            {
                systemError = attributes == INVALID_FILE_ATTRIBUTES ?
                                  GetLastError() : ERROR_REPARSE_TAG_INVALID;
                error = "Task Worktree sidecar is not a safe regular file";
                return false;
            }
            if (DeleteFileW(path.c_str()) == FALSE)
            {
                systemError = GetLastError();
                error = "Task Worktree sidecar could not be deleted";
                return false;
            }
            return true;
        }
    }

    std::optional<Microsoft::Terminal::TaskExecution::GitCommandResult>
    GitWorktreeFinalizeCommandContext::Run(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity& directory,
        std::vector<std::wstring> arguments)
    {
        using namespace Microsoft::Terminal::TaskExecution;
        const auto remaining = _remaining(Deadline);
        if (!remaining)
        {
            FailureStatus = GitWorktreeFinalizeStatus::TimedOut;
            FailureSystemError = WAIT_TIMEOUT;
            FailureError = "Task Worktree finalization command deadline expired";
            return std::nullopt;
        }
        GitCommandRequest request;
        request.GitExecutable = Source.GitExecutable;
        request.WorkingDirectory = directory;
        request.Arguments = std::move(arguments);
        request.CancellationEvent = CancellationEvent;
        request.TimeoutMilliseconds = (std::min)(
            *remaining,
            MaximumGitCommandTimeoutMilliseconds);
        request.MaximumStdoutBytes = GitWorktreeFinalizeStdoutBytes;
        request.MaximumStderrBytes = GitWorktreeFinalizeStderrBytes;
        auto result = Executor(request);
        if (result.Status != GitCommandStatus::Completed)
        {
            FailureStatus = MapGitWorktreeFinalizeCommandStatus(result.Status);
            FailureSystemError = result.SystemError;
            FailureError = result.Error.empty() ?
                               "Task Worktree finalization Git command failed" :
                               result.Error;
            return std::nullopt;
        }
        return result;
    }

    std::optional<Microsoft::Terminal::TaskExecution::GitCommandResult>
    GitWorktreeFinalizeCommandContext::RunSource(
        std::vector<std::wstring> arguments)
    {
        return Run(Source.WorktreeIdentity, std::move(arguments));
    }

    GitWorktreeFinalizeContextResult OpenGitWorktreeFinalizeContext(
        const std::filesystem::path& gitExecutable,
        const Microsoft::Terminal::TaskExecution::GitCommandExecutor& executor,
        const GitWorktreeMarker& marker,
        const std::filesystem::path& repositoryRoot,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) noexcept
    {
        using namespace Microsoft::Terminal::TaskExecution;
        GitWorktreeFinalizeContextResult result;
        try
        {
            if (gitExecutable.empty() || !executor || repositoryRoot.empty() ||
                !cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE ||
                timeoutMilliseconds == 0 ||
                timeoutMilliseconds > MaximumGitWorktreeFinalizeTimeoutMilliseconds ||
                marker.BaselineCommit.empty())
            {
                result.Status = GitWorktreeFinalizeStatus::InvalidRequest;
                result.SystemError = ERROR_INVALID_PARAMETER;
                result.Error = "Task Worktree finalization request is invalid";
                return result;
            }
            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            GitRepositoryInspector inspector{ gitExecutable, executor };
            const auto firstTime = _remaining(deadline);
            if (!firstTime)
            {
                result.Status = GitWorktreeFinalizeStatus::TimedOut;
                result.SystemError = WAIT_TIMEOUT;
                result.Error = "Task Worktree finalization expired before source inspection";
                return result;
            }
            const auto first = inspector.Inspect(
                repositoryRoot,
                cancellationEvent,
                (std::min)(*firstTime, MaximumGitCommandTimeoutMilliseconds));
            if (!first.Succeeded())
            {
                result.Status = MapGitWorktreeFinalizeInspectionStatus(first.Status);
                result.SystemError = first.SystemError;
                result.Error = "Task Worktree finalization repository inspection failed";
                return result;
            }
            if (first.Inspection->CommonDirectoryIdentity.Digest() !=
                    marker.RepositoryIdentityDigest ||
                !IsGitSourceObjectId(
                    marker.BaselineCommit,
                    first.Inspection->ObjectFormat))
            {
                result.Status = GitWorktreeFinalizeStatus::EvidenceMismatch;
                result.SystemError = ERROR_REVISION_MISMATCH;
                result.Error = "Task Worktree finalization repository identity changed";
                return result;
            }
            const auto lockTime = _remaining(deadline);
            if (!lockTime)
            {
                result.Status = GitWorktreeFinalizeStatus::TimedOut;
                result.SystemError = WAIT_TIMEOUT;
                result.Error = "Task Worktree finalization expired before repository locking";
                return result;
            }
            auto lock = AcquireGitRepositoryLock(
                first.Inspection->CommonDirectoryIdentity,
                cancellationEvent,
                (std::min)(*lockTime, GitRepositoryLockTimeoutMilliseconds));
            if (!lock.Acquired())
            {
                result.Status = MapGitWorktreeFinalizeLockStatus(lock.Status);
                result.SystemError = lock.SystemError;
                result.Error = "Task Worktree finalization repository lock failed";
                return result;
            }
            const auto secondTime = _remaining(deadline);
            if (!secondTime)
            {
                result.Status = GitWorktreeFinalizeStatus::TimedOut;
                result.SystemError = WAIT_TIMEOUT;
                result.Error = "Task Worktree finalization expired while locked";
                return result;
            }
            const auto second = inspector.Inspect(
                repositoryRoot,
                cancellationEvent,
                (std::min)(*secondTime, MaximumGitCommandTimeoutMilliseconds));
            if (!second.Succeeded() ||
                second.Inspection->WorktreeIdentity.Digest() !=
                    first.Inspection->WorktreeIdentity.Digest() ||
                second.Inspection->CommonDirectoryIdentity.Digest() !=
                    marker.RepositoryIdentityDigest ||
                second.Inspection->GitDirectoryIdentity.Digest() !=
                    first.Inspection->GitDirectoryIdentity.Digest() ||
                second.Inspection->ObjectFormat != first.Inspection->ObjectFormat)
            {
                result.Status = second.Succeeded() ?
                                    GitWorktreeFinalizeStatus::TargetChanged :
                                    MapGitWorktreeFinalizeInspectionStatus(second.Status);
                result.SystemError = second.SystemError == ERROR_SUCCESS ?
                                         ERROR_REVISION_MISMATCH :
                                         second.SystemError;
                result.Error = "Task Worktree finalization repository changed while locking";
                return result;
            }
            GitWorktreeFinalizeCommandContext context;
            context.Source = *second.Inspection;
            context.Lock = std::move(lock.Lock);
            context.Executor = executor;
            context.CancellationEvent = cancellationEvent;
            context.Deadline = deadline;
            result.Context.emplace(std::move(context));
            result.Status = GitWorktreeFinalizeStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            result.Status = GitWorktreeFinalizeStatus::ResourceFailure;
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error = "Task Worktree finalization context exhausted local resources";
            return result;
        }
    }

    GitWorktreeFinalizeStatus MapGitWorktreeFinalizeCommandStatus(
        const Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitCommandStatus;
        switch (status)
        {
        case GitCommandStatus::Cancelled:
            return GitWorktreeFinalizeStatus::Cancelled;
        case GitCommandStatus::TimedOut:
            return GitWorktreeFinalizeStatus::TimedOut;
        case GitCommandStatus::InvalidRequest:
            return GitWorktreeFinalizeStatus::InvalidRequest;
        case GitCommandStatus::ResourceFailure:
            return GitWorktreeFinalizeStatus::ResourceFailure;
        default:
            return GitWorktreeFinalizeStatus::GitFailure;
        }
    }

    GitWorktreeFinalizeStatus MapGitWorktreeFinalizeInspectionStatus(
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus;
        switch (status)
        {
        case GitRepositoryInspectionStatus::Cancelled:
            return GitWorktreeFinalizeStatus::Cancelled;
        case GitRepositoryInspectionStatus::TimedOut:
            return GitWorktreeFinalizeStatus::TimedOut;
        case GitRepositoryInspectionStatus::LockFailed:
            return GitWorktreeFinalizeStatus::LockFailed;
        case GitRepositoryInspectionStatus::ResourceFailure:
            return GitWorktreeFinalizeStatus::ResourceFailure;
        default:
            return GitWorktreeFinalizeStatus::InspectionFailed;
        }
    }

    GitWorktreeFinalizeStatus MapGitWorktreeFinalizeLockStatus(
        const Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus;
        switch (status)
        {
        case TaskDirectoryLockStatus::Cancelled:
            return GitWorktreeFinalizeStatus::Cancelled;
        case TaskDirectoryLockStatus::TimedOut:
            return GitWorktreeFinalizeStatus::TimedOut;
        case TaskDirectoryLockStatus::ResourceFailure:
            return GitWorktreeFinalizeStatus::ResourceFailure;
        default:
            return GitWorktreeFinalizeStatus::LockFailed;
        }
    }

    std::optional<bool> GitWorktreeFinalizePathExists(
        const std::filesystem::path& path,
        DWORD& systemError) noexcept
    {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            systemError = ERROR_SUCCESS;
            return true;
        }
        systemError = GetLastError();
        if (systemError == ERROR_FILE_NOT_FOUND ||
            systemError == ERROR_PATH_NOT_FOUND)
        {
            systemError = ERROR_SUCCESS;
            return false;
        }
        return std::nullopt;
    }

    GitWorktreeFinalizeRegistrationResult FindGitWorktreeFinalizeRegistration(
        const GitWorktreeLayout& layout,
        const std::span<const GitWorktreeRegistration> registrations) noexcept
    {
        GitWorktreeFinalizeRegistrationResult result;
        try
        {
            for (const auto& registration : registrations)
            {
                const auto samePath = _samePath(
                    registration.Path,
                    layout.WorktreePathUtf8);
                if (!samePath)
                {
                    result.Error = "Git Worktree registration path is invalid";
                    return result;
                }
                if (*samePath)
                {
                    if (result.Managed)
                    {
                        result.Error = "Git repeats the managed Worktree path";
                        return result;
                    }
                    result.Managed = registration;
                }
                else if (registration.BranchRef == layout.BranchRef)
                {
                    result.BranchRegisteredElsewhere = true;
                }
            }
            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            result.Error = "Git Worktree registration parsing exhausted local resources";
            return result;
        }
    }

    GitWorktreeFinalizeResult CleanupGitWorktreeFinalizeSidecars(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& expectedMarker) noexcept
    {
        try
        {
            DWORD systemError{};
            const auto worktreeExists = GitWorktreeFinalizePathExists(
                layout.WorktreePath,
                systemError);
            if (!worktreeExists || *worktreeExists)
            {
                return _sidecarFailure(
                    systemError == ERROR_SUCCESS ? ERROR_DIR_NOT_EMPTY : systemError,
                    "Task Worktree sidecars cannot be removed before the Worktree path");
            }
            const auto marker = ReadGitWorktreeMarker(layout);
            if (marker.Succeeded())
            {
                const auto expectedBytes = EncodeGitWorktreeMarker(expectedMarker);
                const auto actualBytes = EncodeGitWorktreeMarker(*marker.Marker);
                if (!expectedBytes || !actualBytes ||
                    *expectedBytes != *actualBytes)
                {
                    return _sidecarFailure(
                        ERROR_REVISION_MISMATCH,
                        "Task Worktree sidecar ownership marker changed");
                }
            }
            else if (marker.Status != GitWorktreeMarkerStatus::Missing)
            {
                return _sidecarFailure(
                    marker.SystemError,
                    "Task Worktree ownership marker is unsafe or unreadable");
            }

            DWORD snapshotError{};
            DWORD stagingError{};
            const auto snapshotExists = _fileExists(
                layout.SnapshotPath,
                snapshotError);
            const auto stagingExists = _fileExists(
                layout.CopyStagingPath,
                stagingError);
            if (!snapshotExists || !stagingExists)
            {
                return _sidecarFailure(
                    snapshotError != ERROR_SUCCESS ? snapshotError : stagingError,
                    "Task Worktree sidecar state is unavailable");
            }
            if (marker.Status == GitWorktreeMarkerStatus::Missing &&
                (*snapshotExists || *stagingExists))
            {
                return _sidecarFailure(
                    ERROR_REVISION_MISMATCH,
                    "Task Worktree sidecars remain without ownership proof");
            }

            std::string error;
            if (!_deleteRegularSidecar(
                    layout.CopyStagingPath,
                    systemError,
                    error) ||
                !_deleteRegularSidecar(
                    layout.SnapshotPath,
                    systemError,
                    error) ||
                !_deleteRegularSidecar(
                    layout.MarkerPath,
                    systemError,
                    error))
            {
                return _sidecarFailure(systemError, std::move(error));
            }
            GitWorktreeFinalizeResult result;
            result.Status = GitWorktreeFinalizeStatus::Succeeded;
            result.WorktreeRemoved = true;
            result.SidecarsRemoved = true;
            return result;
        }
        catch (...)
        {
            return _sidecarFailure(
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree sidecar cleanup exhausted local resources");
        }
    }

    std::string GitWorktreeFinalizeSubject(const GitWorktreeMarker& marker)
    {
        return "Richer Task finalization " + marker.Task.Value() + " " +
               marker.OwnershipNonce;
    }
}
