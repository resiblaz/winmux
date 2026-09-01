// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeFileCopy.h"

#include "GitSourceCapturePolicy.h"
#include "GitWorktreeFileCopyInternal.h"

namespace Microsoft::Terminal::TaskGit
{
    GitWorktreeFileCopyResult CopyGitWorktreeSnapshotFile(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            sourceRoot,
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            destinationRoot,
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity&
            stateRoot,
        const std::filesystem::path& stagingPath,
        const GitSourceFileRecord& file,
        const HANDLE cancellationEvent) noexcept
    {
        using namespace Microsoft::Terminal::TaskExecution;
        try
        {
            const auto relative =
                details::DecodeGitWorktreeCopyRelativePath(file.RelativePath);
            if (!details::ValidateGitWorktreeCopyRoot(sourceRoot) ||
                !details::ValidateGitWorktreeCopyRoot(destinationRoot) ||
                !details::ValidateGitWorktreeCopyRoot(stateRoot) || !relative ||
                stagingPath.empty() ||
                stagingPath.parent_path() != stateRoot.CanonicalPath() ||
                !cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE ||
                file.Size > MaximumGitSnapshotUntrackedBytes ||
                !details::ValidateGitSourceFileRecord(
                    file,
                    file.RelativePath))
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Task Worktree file copy request is invalid");
            }
            const auto sourcePath =
                (sourceRoot.CanonicalPath() / *relative).lexically_normal();
            const auto destinationPath =
                (destinationRoot.CanonicalPath() / *relative).lexically_normal();
            if (sourcePath.native().size() > MaximumTaskExecutionPathCharacters ||
                destinationPath.native().size() >
                    MaximumTaskExecutionPathCharacters ||
                stagingPath.native().size() > MaximumTaskExecutionPathCharacters)
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::InvalidRequest,
                    ERROR_FILENAME_EXCED_RANGE,
                    "Task Worktree file copy path exceeds its bound");
            }

            const auto parents = details::EnsureGitWorktreeCopyParents(
                destinationRoot,
                relative->parent_path());
            if (!parents.Succeeded())
            {
                return parents;
            }
            const auto existing = details::InspectGitWorktreeCopyDestination(
                destinationPath,
                file,
                cancellationEvent);
            if (!existing.Missing)
            {
                return existing.Result;
            }
            const auto cleaned = details::CleanupGitWorktreeCopyStaging(
                stagingPath);
            if (!cleaned.Succeeded())
            {
                return cleaned;
            }

            details::GitWorktreeCopyHandle source;
            source.Value = CreateFileW(
                details::ExtendedGitWorktreeCopyPath(sourcePath).c_str(),
                GENERIC_READ | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                    FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (!source.Value || source.Value == INVALID_HANDLE_VALUE)
            {
                source.Value = nullptr;
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::SourceChanged,
                    GetLastError(),
                    "Captured source file is no longer available");
            }
            details::GitWorktreeFileEvidence sourceBefore;
            DWORD error{};
            if (!details::ReadGitWorktreeFileEvidence(
                    source.Value,
                    sourcePath,
                    sourceBefore,
                    error))
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::UnsafeSource,
                    error,
                    "Captured source file identity is unsafe");
            }
            if (sourceBefore.Standard.EndOfFile.QuadPart < 0 ||
                static_cast<uint64_t>(
                    sourceBefore.Standard.EndOfFile.QuadPart) != file.Size)
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::SourceChanged,
                    ERROR_FILE_INVALID,
                    "Captured source file size no longer matches");
            }
            const auto sourceHash = details::HashOpenGitFileSha256(
                source.Value,
                file.Size,
                cancellationEvent);
            if (!sourceHash.Succeeded())
            {
                return details::MapGitWorktreeCopyHashFailure(
                    sourceHash,
                    GitWorktreeFileCopyStatus::SourceChanged,
                    "Captured source file could not be verified");
            }
            if (sourceHash.Digest != file.Sha256)
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::SourceChanged,
                    ERROR_FILE_INVALID,
                    "Captured source file digest no longer matches");
            }
            if (!details::RewindGitWorktreeCopyFile(source.Value, error))
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::IoFailure,
                    error,
                    "Captured source file could not be rewound");
            }

            details::GitWorktreeCopyHandle staging;
            staging.Value = CreateFileW(
                details::ExtendedGitWorktreeCopyPath(stagingPath).c_str(),
                GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                    FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (!staging.Value || staging.Value == INVALID_HANDLE_VALUE)
            {
                staging.Value = nullptr;
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::IoFailure,
                    GetLastError(),
                    "Task Worktree staging file could not be created");
            }
            details::GitWorktreeFileEvidence stagingInitial;
            if (!details::ReadGitWorktreeFileEvidence(
                    staging.Value,
                    stagingPath,
                    stagingInitial,
                    error))
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::UnsafeDestination,
                    error,
                    "Task Worktree staging file identity is unsafe");
            }
            const auto copied = details::CopyGitWorktreeFileBytes(
                source.Value,
                staging.Value,
                file.Size,
                cancellationEvent);
            if (!copied.Succeeded())
            {
                return copied;
            }
            if (!FlushFileBuffers(staging.Value))
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::IoFailure,
                    GetLastError(),
                    "Task Worktree staging file could not be flushed");
            }

            details::GitWorktreeFileEvidence sourceAfter;
            details::GitWorktreeFileEvidence stagingAfter;
            if (!details::ReadGitWorktreeFileEvidence(
                    source.Value,
                    sourcePath,
                    sourceAfter,
                    error) ||
                !details::SameGitWorktreeFileEvidence(
                    sourceBefore,
                    sourceAfter))
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::SourceChanged,
                    error == ERROR_SUCCESS ? ERROR_FILE_INVALID : error,
                    "Captured source file changed during copy");
            }
            if (!details::ReadGitWorktreeFileEvidence(
                    staging.Value,
                    stagingPath,
                    stagingAfter,
                    error) ||
                stagingAfter.Standard.EndOfFile.QuadPart < 0 ||
                static_cast<uint64_t>(
                    stagingAfter.Standard.EndOfFile.QuadPart) != file.Size ||
                !details::RewindGitWorktreeCopyFile(staging.Value, error))
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::IoFailure,
                    error == ERROR_SUCCESS ? ERROR_FILE_INVALID : error,
                    "Task Worktree staging file verification failed");
            }
            const auto stagingHash = details::HashOpenGitFileSha256(
                staging.Value,
                file.Size,
                cancellationEvent);
            if (!stagingHash.Succeeded())
            {
                return details::MapGitWorktreeCopyHashFailure(
                    stagingHash,
                    GitWorktreeFileCopyStatus::IoFailure,
                    "Task Worktree staging file could not be hashed");
            }
            if (stagingHash.Digest != file.Sha256)
            {
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::IoFailure,
                    ERROR_CRC,
                    "Task Worktree staging digest does not match its manifest");
            }
            staging.Reset();

            if (!MoveFileExW(
                    details::ExtendedGitWorktreeCopyPath(stagingPath).c_str(),
                    details::ExtendedGitWorktreeCopyPath(destinationPath).c_str(),
                    MOVEFILE_WRITE_THROUGH))
            {
                const auto moveError = GetLastError();
                if (moveError == ERROR_FILE_EXISTS ||
                    moveError == ERROR_ALREADY_EXISTS)
                {
                    const auto cleanup =
                        details::CleanupGitWorktreeCopyStaging(stagingPath);
                    if (!cleanup.Succeeded())
                    {
                        return cleanup;
                    }
                    const auto raced =
                        details::InspectGitWorktreeCopyDestination(
                            destinationPath,
                            file,
                            cancellationEvent);
                    return raced.Missing ?
                               details::GitWorktreeFileCopyResultFor(
                                   GitWorktreeFileCopyStatus::IoFailure,
                                   ERROR_FILE_NOT_FOUND,
                                   "Task Worktree destination disappeared after collision") :
                               raced.Result;
                }
                return details::GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::IoFailure,
                    moveError,
                    "Task Worktree staging file could not be committed atomically");
            }

            const auto verified = details::InspectGitWorktreeCopyDestination(
                destinationPath,
                file,
                cancellationEvent);
            if (verified.Missing || !verified.Result.Succeeded())
            {
                return verified.Missing ?
                           details::GitWorktreeFileCopyResultFor(
                               GitWorktreeFileCopyStatus::IoFailure,
                               ERROR_FILE_NOT_FOUND,
                               "Copied Task Worktree destination disappeared") :
                           verified.Result;
            }
            return details::GitWorktreeFileCopyResultFor(
                GitWorktreeFileCopyStatus::Succeeded,
                ERROR_SUCCESS,
                {});
        }
        catch (...)
        {
            return details::GitWorktreeFileCopyResultFor(
                GitWorktreeFileCopyStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree file copy exhausted local resources");
        }
    }
}
