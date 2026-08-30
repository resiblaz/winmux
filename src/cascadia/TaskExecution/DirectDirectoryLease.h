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
        struct DirectDirectoryLeaseData;
    }

    struct DirectDirectoryLeaseResult;

    class DirectDirectoryLease final
    {
    public:
        DirectDirectoryLease() noexcept;
        ~DirectDirectoryLease();

        DirectDirectoryLease(const DirectDirectoryLease&) = delete;
        DirectDirectoryLease& operator=(const DirectDirectoryLease&) = delete;
        DirectDirectoryLease(DirectDirectoryLease&& other) noexcept;
        DirectDirectoryLease& operator=(DirectDirectoryLease&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] bool Abandoned() const noexcept;
        [[nodiscard]] DWORD OwnerThreadId() const noexcept;
        // Borrowed for diagnostics/tests; never inherit or close this handle.
        [[nodiscard]] HANDLE Handle() const noexcept;
        [[nodiscard]] const std::wstring& MutexName() const noexcept;
        [[nodiscard]] TaskDirectoryLockReleaseStatus Release() noexcept;

    private:
        friend struct DirectDirectoryLeaseResult;
        friend class DirectDirectoryLeaseManager;

        std::unique_ptr<details::DirectDirectoryLeaseData> _data;
    };

    struct DirectDirectoryLeaseResult
    {
        TaskDirectoryLockStatus Status{ TaskDirectoryLockStatus::ResourceFailure };
        DirectDirectoryLease Lease;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Acquired() const noexcept
        {
            return Status == TaskDirectoryLockStatus::Acquired &&
                   static_cast<bool>(Lease);
        }
    };

    class DirectDirectoryLeaseManager final
    {
    public:
        DirectDirectoryLeaseManager();
        ~DirectDirectoryLeaseManager();

        DirectDirectoryLeaseManager(const DirectDirectoryLeaseManager&) = delete;
        DirectDirectoryLeaseManager& operator=(const DirectDirectoryLeaseManager&) = delete;
        DirectDirectoryLeaseManager(DirectDirectoryLeaseManager&&) = delete;
        DirectDirectoryLeaseManager& operator=(DirectDirectoryLeaseManager&&) = delete;

        [[nodiscard]] DirectDirectoryLeaseResult Acquire(
            const CanonicalDirectoryIdentity& directory,
            HANDLE cancellationEvent) noexcept;
        [[nodiscard]] size_t ParticipantCount(
            const CanonicalDirectoryIdentity& directory) const noexcept;

    private:
        struct Impl;
        [[nodiscard]] static std::shared_ptr<Impl> _sharedImpl();

        std::shared_ptr<Impl> _impl;
    };
}
