// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeFileCopyInternal.h"

namespace Microsoft::Terminal::TaskGit::details
{
    GitWorktreeFileCopyResult MapGitWorktreeCopyHashFailure(
        const GitFileHashResult& hash,
        const GitWorktreeFileCopyStatus failureStatus,
        std::string error)
    {
        return GitWorktreeFileCopyResultFor(
            hash.Status == GitFileHashStatus::Cancelled ?
                GitWorktreeFileCopyStatus::Cancelled :
            hash.Status == GitFileHashStatus::ResourceFailure ?
                GitWorktreeFileCopyStatus::ResourceFailure :
                failureStatus,
            hash.SystemError,
            std::move(error));
    }

    GitWorktreeDestinationInspection InspectGitWorktreeCopyDestination(
        const std::filesystem::path& destination,
        const GitSourceFileRecord& expected,
        const HANDLE cancellationEvent)
    {
        const auto attributes = GetFileAttributesW(
            ExtendedGitWorktreeCopyPath(destination).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            {
                return { true, {} };
            }
            return {
                false,
                GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::IoFailure,
                    error,
                    "Task Worktree destination could not be inspected"),
            };
        }

        GitWorktreeCopyHandle file;
        file.Value = CreateFileW(
            ExtendedGitWorktreeCopyPath(destination).c_str(),
            GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (!file.Value || file.Value == INVALID_HANDLE_VALUE)
        {
            file.Value = nullptr;
            return {
                false,
                GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::DestinationCollision,
                    GetLastError(),
                    "Task Worktree destination could not be opened safely"),
            };
        }
        GitWorktreeFileEvidence before;
        DWORD error{};
        if (!ReadGitWorktreeFileEvidence(
                file.Value,
                destination,
                before,
                error))
        {
            return {
                false,
                GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::UnsafeDestination,
                    error,
                    "Task Worktree destination identity is unsafe"),
            };
        }
        if (before.Standard.EndOfFile.QuadPart < 0 ||
            static_cast<uint64_t>(before.Standard.EndOfFile.QuadPart) !=
                expected.Size)
        {
            return {
                false,
                GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::DestinationCollision,
                    ERROR_FILE_EXISTS,
                    "Task Worktree destination has different content"),
            };
        }
        const auto hash = HashOpenGitFileSha256(
            file.Value,
            expected.Size,
            cancellationEvent);
        if (!hash.Succeeded())
        {
            return {
                false,
                MapGitWorktreeCopyHashFailure(
                    hash,
                    GitWorktreeFileCopyStatus::DestinationCollision,
                    "Task Worktree destination could not be verified"),
            };
        }
        GitWorktreeFileEvidence after;
        if (!ReadGitWorktreeFileEvidence(
                file.Value,
                destination,
                after,
                error) ||
            !SameGitWorktreeFileEvidence(before, after))
        {
            return {
                false,
                GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::DestinationCollision,
                    error == ERROR_SUCCESS ? ERROR_FILE_INVALID : error,
                    "Task Worktree destination changed during verification"),
            };
        }
        return {
            false,
            GitWorktreeFileCopyResultFor(
                hash.Digest == expected.Sha256 ?
                    GitWorktreeFileCopyStatus::AlreadyPresent :
                    GitWorktreeFileCopyStatus::DestinationCollision,
                hash.Digest == expected.Sha256 ?
                    ERROR_SUCCESS :
                    ERROR_FILE_EXISTS,
                hash.Digest == expected.Sha256 ?
                    std::string{} :
                    "Task Worktree destination digest does not match"),
        };
    }

    GitWorktreeFileCopyResult CopyGitWorktreeFileBytes(
        const HANDLE source,
        const HANDLE destination,
        const uint64_t expectedBytes,
        const HANDLE cancellationEvent)
    {
        uint64_t total{};
        std::array<uint8_t, 64 * 1024> buffer{};
        for (;;)
        {
            const auto cancelled = WaitForSingleObject(cancellationEvent, 0);
            if (cancelled == WAIT_OBJECT_0)
            {
                return GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::Cancelled,
                    ERROR_OPERATION_ABORTED,
                    "Task Worktree file copy was cancelled");
            }
            if (cancelled != WAIT_TIMEOUT)
            {
                return GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::IoFailure,
                    cancelled == WAIT_FAILED ?
                        GetLastError() :
                        ERROR_INVALID_HANDLE,
                    "Task Worktree file copy cancellation handle failed");
            }
            DWORD read{};
            if (!ReadFile(
                    source,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr))
            {
                return GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::SourceChanged,
                    GetLastError(),
                    "Captured source file could not be read");
            }
            if (read == 0)
            {
                break;
            }
            if (total > expectedBytes || read > expectedBytes - total)
            {
                return GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::SourceChanged,
                    ERROR_FILE_INVALID,
                    "Captured source file grew during copy");
            }
            DWORD offset{};
            while (offset < read)
            {
                DWORD written{};
                if (!WriteFile(
                        destination,
                        buffer.data() + offset,
                        read - offset,
                        &written,
                        nullptr) ||
                    written == 0)
                {
                    return GitWorktreeFileCopyResultFor(
                        GitWorktreeFileCopyStatus::IoFailure,
                        GetLastError(),
                        "Task Worktree staging file could not be written");
                }
                offset += written;
            }
            total += read;
        }
        if (total != expectedBytes)
        {
            return GitWorktreeFileCopyResultFor(
                GitWorktreeFileCopyStatus::SourceChanged,
                ERROR_FILE_INVALID,
                "Captured source file size changed during copy");
        }
        return GitWorktreeFileCopyResultFor(
            GitWorktreeFileCopyStatus::Succeeded,
            ERROR_SUCCESS,
            {});
    }
}
