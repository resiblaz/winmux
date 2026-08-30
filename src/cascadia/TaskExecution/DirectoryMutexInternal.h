// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "DirectoryIdentity.h"
#include "TaskDirectoryLock.h"

#include <windows.h>

#include <string>

namespace Microsoft::Terminal::TaskExecution::details
{
    enum class DirectoryMutexScope : uint8_t
    {
        Direct,
        Git,
    };

    struct DirectoryMutexIdentityResult
    {
        TaskDirectoryLockStatus Status{ TaskDirectoryLockStatus::ResourceFailure };
        std::wstring MutexName;
        std::wstring UserSid;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == TaskDirectoryLockStatus::Acquired &&
                   !MutexName.empty() && !UserSid.empty();
        }
    };

    struct DirectoryMutexAcquireResult;

    class DirectoryMutexLease final
    {
    public:
        DirectoryMutexLease() noexcept = default;
        ~DirectoryMutexLease();

        DirectoryMutexLease(const DirectoryMutexLease&) = delete;
        DirectoryMutexLease& operator=(const DirectoryMutexLease&) = delete;
        DirectoryMutexLease(DirectoryMutexLease&& other) noexcept;
        DirectoryMutexLease& operator=(DirectoryMutexLease&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] bool Abandoned() const noexcept;
        [[nodiscard]] DWORD OwnerThreadId() const noexcept;
        [[nodiscard]] HANDLE Handle() const noexcept;
        [[nodiscard]] const std::wstring& MutexName() const noexcept;
        [[nodiscard]] TaskDirectoryLockReleaseStatus Release() noexcept;

    private:
        friend struct DirectoryMutexAcquireResult;
        friend DirectoryMutexAcquireResult AcquireDirectoryMutex(
            const CanonicalDirectoryIdentity&,
            DirectoryMutexScope,
            DWORD,
            HANDLE) noexcept;

        void _close() noexcept;

        HANDLE _handle{};
        DWORD _ownerThreadId{};
        bool _owns{};
        bool _abandoned{};
        std::wstring _mutexName;
    };

    struct DirectoryMutexAcquireResult
    {
        TaskDirectoryLockStatus Status{ TaskDirectoryLockStatus::ResourceFailure };
        DirectoryMutexLease Lease;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Acquired() const noexcept
        {
            return Status == TaskDirectoryLockStatus::Acquired &&
                   static_cast<bool>(Lease);
        }
    };

    [[nodiscard]] DirectoryMutexIdentityResult BuildDirectoryMutexIdentity(
        const CanonicalDirectoryIdentity& identity,
        DirectoryMutexScope scope) noexcept;
    [[nodiscard]] DirectoryMutexAcquireResult AcquireDirectoryMutex(
        const CanonicalDirectoryIdentity& identity,
        DirectoryMutexScope scope,
        DWORD timeoutMilliseconds,
        HANDLE cancellationEvent) noexcept;
}
