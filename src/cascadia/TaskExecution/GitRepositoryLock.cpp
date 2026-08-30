// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitRepositoryLock.h"

#include "DirectoryMutexInternal.h"

namespace Microsoft::Terminal::TaskExecution
{
    GitRepositoryLock::GitRepositoryLock() noexcept = default;
    GitRepositoryLock::~GitRepositoryLock() = default;

    GitRepositoryLock::GitRepositoryLock(GitRepositoryLock&& other) noexcept
    {
        if (other._lease &&
            other._lease->OwnerThreadId() != GetCurrentThreadId())
        {
            std::terminate();
        }
        _lease = std::move(other._lease);
    }

    GitRepositoryLock& GitRepositoryLock::operator=(GitRepositoryLock&& other) noexcept
    {
        if (this != &other)
        {
            if ((_lease && _lease->OwnerThreadId() != GetCurrentThreadId()) ||
                (other._lease &&
                 other._lease->OwnerThreadId() != GetCurrentThreadId()))
            {
                std::terminate();
            }
            _lease = std::move(other._lease);
        }
        return *this;
    }

    GitRepositoryLock::operator bool() const noexcept
    {
        return _lease && static_cast<bool>(*_lease);
    }

    bool GitRepositoryLock::Abandoned() const noexcept
    {
        return _lease && _lease->Abandoned();
    }

    DWORD GitRepositoryLock::OwnerThreadId() const noexcept
    {
        return _lease ? _lease->OwnerThreadId() : 0;
    }

    HANDLE GitRepositoryLock::Handle() const noexcept
    {
        return _lease ? _lease->Handle() : nullptr;
    }

    const std::wstring& GitRepositoryLock::MutexName() const noexcept
    {
        static const std::wstring empty;
        return _lease ? _lease->MutexName() : empty;
    }

    TaskDirectoryLockReleaseStatus GitRepositoryLock::Release() noexcept
    {
        if (!_lease)
        {
            return TaskDirectoryLockReleaseStatus::NotOwned;
        }
        const auto status = _lease->Release();
        if (status == TaskDirectoryLockReleaseStatus::Released ||
            status == TaskDirectoryLockReleaseStatus::NotOwned)
        {
            _lease.reset();
        }
        return status;
    }

    GitRepositoryLockResult AcquireGitRepositoryLock(
        const CanonicalDirectoryIdentity& commonDirectory,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) noexcept
    {
        auto acquired = details::AcquireDirectoryMutex(
            commonDirectory,
            details::DirectoryMutexScope::Git,
            timeoutMilliseconds,
            cancellationEvent);
        GitRepositoryLockResult result;
        result.Status = acquired.Status;
        result.SystemError = acquired.SystemError;
        result.Error = std::move(acquired.Error);
        if (!acquired.Acquired())
        {
            return result;
        }
        try
        {
            result.Lock._lease =
                std::make_unique<details::DirectoryMutexLease>(
                    std::move(acquired.Lease));
            return result;
        }
        catch (const std::exception&)
        {
            result.Status = TaskDirectoryLockStatus::ResourceFailure;
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error = "Git repository lock exhausted local resources";
            return result;
        }
        catch (...)
        {
            result.Status = TaskDirectoryLockStatus::ResourceFailure;
            result.SystemError = ERROR_UNHANDLED_EXCEPTION;
            result.Error = "Unexpected Git repository lock allocation failure";
            return result;
        }
    }
}
