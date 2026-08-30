// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "DirectoryIdentity.h"
#include "TaskDirectoryLock.h"

#include <windows.h>

#include <memory>
#include <string>

namespace Microsoft::Terminal::TaskExecution
{
    namespace details
    {
        class DirectoryMutexLease;
    }

    struct GitRepositoryLockResult;

    class GitRepositoryLock final
    {
    public:
        GitRepositoryLock() noexcept;
        ~GitRepositoryLock();

        GitRepositoryLock(const GitRepositoryLock&) = delete;
        GitRepositoryLock& operator=(const GitRepositoryLock&) = delete;
        GitRepositoryLock(GitRepositoryLock&& other) noexcept;
        GitRepositoryLock& operator=(GitRepositoryLock&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] bool Abandoned() const noexcept;
        [[nodiscard]] DWORD OwnerThreadId() const noexcept;
        // Borrowed for diagnostics/tests; never inherit or close this handle.
        [[nodiscard]] HANDLE Handle() const noexcept;
        [[nodiscard]] const std::wstring& MutexName() const noexcept;
        [[nodiscard]] TaskDirectoryLockReleaseStatus Release() noexcept;

    private:
        friend struct GitRepositoryLockResult;
        friend GitRepositoryLockResult AcquireGitRepositoryLock(
            const CanonicalDirectoryIdentity&,
            HANDLE,
            uint32_t) noexcept;

        std::unique_ptr<details::DirectoryMutexLease> _lease;
    };

    struct GitRepositoryLockResult
    {
        TaskDirectoryLockStatus Status{ TaskDirectoryLockStatus::ResourceFailure };
        GitRepositoryLock Lock;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Acquired() const noexcept
        {
            return Status == TaskDirectoryLockStatus::Acquired &&
                   static_cast<bool>(Lock);
        }
    };

    [[nodiscard]] GitRepositoryLockResult AcquireGitRepositoryLock(
        const CanonicalDirectoryIdentity& commonDirectory,
        HANDLE cancellationEvent,
        uint32_t timeoutMilliseconds = GitRepositoryLockTimeoutMilliseconds) noexcept;
}
