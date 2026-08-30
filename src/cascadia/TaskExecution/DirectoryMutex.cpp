// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "DirectoryMutexInternal.h"

#include "TaskExecutionCrypto.h"
#include "TaskExecutionSecurity.h"

namespace Microsoft::Terminal::TaskExecution::details
{
    namespace
    {
        DirectoryMutexIdentityResult _identityFailure(
            const TaskDirectoryLockStatus status,
            const DWORD systemError,
            std::string error)
        {
            DirectoryMutexIdentityResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        DirectoryMutexAcquireResult _acquireFailure(
            const TaskDirectoryLockStatus status,
            const DWORD systemError,
            std::string error)
        {
            DirectoryMutexAcquireResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        struct MutexHandleOwner final
        {
            MutexHandleOwner() noexcept = default;
            MutexHandleOwner(const MutexHandleOwner&) = delete;
            MutexHandleOwner& operator=(const MutexHandleOwner&) = delete;

            ~MutexHandleOwner()
            {
                if (Value && Value != INVALID_HANDLE_VALUE)
                {
                    if (Owns)
                    {
                        (void)ReleaseMutex(Value);
                    }
                    CloseHandle(Value);
                }
            }

            [[nodiscard]] HANDLE Detach() noexcept
            {
                Owns = false;
                return std::exchange(Value, nullptr);
            }

            HANDLE Value{};
            bool Owns{};
        };

        bool _validCancellationHandle(
            const HANDLE cancellationEvent,
            DWORD& systemError) noexcept
        {
            if (!cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE)
            {
                systemError = ERROR_INVALID_HANDLE;
                return false;
            }
            const auto waited = WaitForSingleObject(cancellationEvent, 0);
            if (waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT)
            {
                return true;
            }
            systemError = waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE;
            return false;
        }

        std::vector<uint8_t> _mutexIdentityPayload(
            const CanonicalDirectoryIdentity& identity,
            const DirectoryMutexScope scope,
            const std::wstring_view userSid)
        {
            constexpr std::string_view domain{ "wt-task-directory-mutex-v1" };
            const std::string_view scopeName = scope == DirectoryMutexScope::Direct ?
                                                   "direct" :
                                                   "git-common-dir";
            std::vector<uint8_t> payload;
            payload.reserve(
                domain.size() + scopeName.size() + identity.Digest().size() +
                userSid.size() * sizeof(wchar_t) + 4);
            payload.insert(payload.end(), domain.begin(), domain.end());
            payload.push_back(0);
            payload.insert(payload.end(), scopeName.begin(), scopeName.end());
            payload.push_back(0);
            payload.insert(
                payload.end(),
                identity.Digest().begin(),
                identity.Digest().end());
            payload.push_back(0);
            const auto* sidBytes = reinterpret_cast<const uint8_t*>(userSid.data());
            payload.insert(
                payload.end(),
                sidBytes,
                sidBytes + userSid.size() * sizeof(wchar_t));
            payload.push_back(0);
            return payload;
        }
    }

    DirectoryMutexLease::~DirectoryMutexLease()
    {
        _close();
    }

    DirectoryMutexLease::DirectoryMutexLease(DirectoryMutexLease&& other) noexcept :
        _handle{ std::exchange(other._handle, nullptr) },
        _ownerThreadId{ std::exchange(other._ownerThreadId, 0) },
        _owns{ std::exchange(other._owns, false) },
        _abandoned{ std::exchange(other._abandoned, false) },
        _mutexName{ std::move(other._mutexName) }
    {
        if (_owns && GetCurrentThreadId() != _ownerThreadId)
        {
            std::terminate();
        }
    }

    DirectoryMutexLease& DirectoryMutexLease::operator=(DirectoryMutexLease&& other) noexcept
    {
        if (this != &other)
        {
            const auto currentThread = GetCurrentThreadId();
            if ((_owns && currentThread != _ownerThreadId) ||
                (other._owns && currentThread != other._ownerThreadId))
            {
                std::terminate();
            }
            _close();
            _handle = std::exchange(other._handle, nullptr);
            _ownerThreadId = std::exchange(other._ownerThreadId, 0);
            _owns = std::exchange(other._owns, false);
            _abandoned = std::exchange(other._abandoned, false);
            _mutexName = std::move(other._mutexName);
        }
        return *this;
    }

    DirectoryMutexLease::operator bool() const noexcept
    {
        return _owns && _handle && _handle != INVALID_HANDLE_VALUE;
    }

    bool DirectoryMutexLease::Abandoned() const noexcept
    {
        return static_cast<bool>(*this) && _abandoned;
    }

    DWORD DirectoryMutexLease::OwnerThreadId() const noexcept
    {
        return static_cast<bool>(*this) ? _ownerThreadId : 0;
    }

    HANDLE DirectoryMutexLease::Handle() const noexcept
    {
        return static_cast<bool>(*this) ? _handle : nullptr;
    }

    const std::wstring& DirectoryMutexLease::MutexName() const noexcept
    {
        return _mutexName;
    }

    TaskDirectoryLockReleaseStatus DirectoryMutexLease::Release() noexcept
    {
        if (!static_cast<bool>(*this))
        {
            return TaskDirectoryLockReleaseStatus::NotOwned;
        }
        if (GetCurrentThreadId() != _ownerThreadId)
        {
            return TaskDirectoryLockReleaseStatus::WrongThread;
        }
        if (!ReleaseMutex(_handle))
        {
            return TaskDirectoryLockReleaseStatus::SystemError;
        }
        CloseHandle(_handle);
        _handle = nullptr;
        _ownerThreadId = 0;
        _owns = false;
        _abandoned = false;
        _mutexName.clear();
        return TaskDirectoryLockReleaseStatus::Released;
    }

    void DirectoryMutexLease::_close() noexcept
    {
        if (!_handle || _handle == INVALID_HANDLE_VALUE)
        {
            return;
        }
        if (_owns && GetCurrentThreadId() != _ownerThreadId)
        {
            std::terminate();
        }
        if (_owns)
        {
            (void)ReleaseMutex(_handle);
        }
        CloseHandle(_handle);
        _handle = nullptr;
        _ownerThreadId = 0;
        _owns = false;
        _abandoned = false;
        _mutexName.clear();
    }

    DirectoryMutexIdentityResult BuildDirectoryMutexIdentity(
        const CanonicalDirectoryIdentity& identity,
        const DirectoryMutexScope scope) noexcept
    {
        try
        {
            if (!identity.Valid() ||
                (scope != DirectoryMutexScope::Direct &&
                 scope != DirectoryMutexScope::Git) ||
                (scope == DirectoryMutexScope::Direct &&
                 identity.Policy() != DirectoryIdentityPolicy::Direct) ||
                (scope == DirectoryMutexScope::Git &&
                 identity.Policy() != DirectoryIdentityPolicy::Worktree))
            {
                return _identityFailure(
                    TaskDirectoryLockStatus::InvalidIdentity,
                    ERROR_INVALID_PARAMETER,
                    "Task directory Mutex identity is invalid for its scope");
            }

            std::string error;
            const auto userSid = CurrentTaskExecutionUserSid(error);
            if (!userSid)
            {
                return _identityFailure(
                    TaskDirectoryLockStatus::SecurityFailure,
                    ERROR_NO_TOKEN,
                    std::move(error));
            }
            const auto payload = _mutexIdentityPayload(identity, scope, *userSid);
            const auto digest = TaskExecutionSha256(payload);
            if (!digest)
            {
                return _identityFailure(
                    TaskDirectoryLockStatus::ResourceFailure,
                    ERROR_INVALID_DATA,
                    "Task directory Mutex identity digest failed");
            }
            const auto hex = LowerHex(*digest);
            const std::wstring wideHex{ hex.begin(), hex.end() };

            DirectoryMutexIdentityResult result;
            result.Status = TaskDirectoryLockStatus::Acquired;
            result.UserSid = *userSid;
            result.MutexName = scope == DirectoryMutexScope::Direct ?
                                   L"Global\\WT.TaskExecution.Direct.v1." + wideHex :
                                   L"Global\\WT.TaskExecution.Git.v1." + wideHex;
            return result;
        }
        catch (const std::exception&)
        {
            return _identityFailure(
                TaskDirectoryLockStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task directory Mutex identity exhausted local resources");
        }
        catch (...)
        {
            return _identityFailure(
                TaskDirectoryLockStatus::ResourceFailure,
                ERROR_UNHANDLED_EXCEPTION,
                "Unexpected Task directory Mutex identity failure");
        }
    }

    DirectoryMutexAcquireResult AcquireDirectoryMutex(
        const CanonicalDirectoryIdentity& identity,
        const DirectoryMutexScope scope,
        const DWORD timeoutMilliseconds,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            if ((scope == DirectoryMutexScope::Direct &&
                 timeoutMilliseconds != INFINITE) ||
                (scope == DirectoryMutexScope::Git &&
                 (timeoutMilliseconds == 0 ||
                  timeoutMilliseconds > GitRepositoryLockTimeoutMilliseconds)))
            {
                return _acquireFailure(
                    TaskDirectoryLockStatus::InvalidTimeout,
                    ERROR_INVALID_PARAMETER,
                    "Task directory Mutex timeout is invalid for its scope");
            }
            DWORD error{};
            if (!_validCancellationHandle(cancellationEvent, error))
            {
                return _acquireFailure(
                    TaskDirectoryLockStatus::InvalidCancellation,
                    error,
                    "Task directory Mutex cancellation handle is invalid");
            }

            const auto mutexIdentity = BuildDirectoryMutexIdentity(identity, scope);
            if (!mutexIdentity.Succeeded())
            {
                return _acquireFailure(
                    mutexIdentity.Status,
                    mutexIdentity.SystemError,
                    mutexIdentity.Error);
            }
            auto security = CreateTaskExecutionSecurityAttributes(
                mutexIdentity.UserSid);
            if (!security.Succeeded)
            {
                return _acquireFailure(
                    TaskDirectoryLockStatus::SecurityFailure,
                    security.SystemError,
                    std::move(security.Error));
            }

            SetLastError(ERROR_SUCCESS);
            MutexHandleOwner mutex;
            mutex.Value = CreateMutexW(
                const_cast<SECURITY_ATTRIBUTES*>(security.Security.Attributes()),
                FALSE,
                mutexIdentity.MutexName.c_str());
            if (!mutex.Value)
            {
                error = GetLastError();
                return _acquireFailure(
                    error == ERROR_ACCESS_DENIED ?
                        TaskDirectoryLockStatus::SecurityFailure :
                        TaskDirectoryLockStatus::SystemError,
                    error,
                    "Task directory Mutex could not be created or opened");
            }
            if (!SetHandleInformation(mutex.Value, HANDLE_FLAG_INHERIT, 0))
            {
                error = GetLastError();
                return _acquireFailure(
                    TaskDirectoryLockStatus::SystemError,
                    error,
                    "Task directory Mutex handle could not be made non-inheritable");
            }
            const auto validated = ValidateTaskExecutionMutexSecurity(
                mutex.Value,
                mutexIdentity.UserSid);
            if (!validated.Succeeded)
            {
                return _acquireFailure(
                    TaskDirectoryLockStatus::SecurityFailure,
                    validated.SystemError,
                    std::move(validated.Error));
            }

            std::array<HANDLE, 2> handles{ cancellationEvent, mutex.Value };
            const auto waited = WaitForMultipleObjects(
                static_cast<DWORD>(handles.size()),
                handles.data(),
                FALSE,
                timeoutMilliseconds);
            if (waited == WAIT_OBJECT_0)
            {
                return _acquireFailure(
                    TaskDirectoryLockStatus::Cancelled,
                    ERROR_OPERATION_ABORTED,
                    "Task directory Mutex wait was cancelled");
            }
            if (waited == WAIT_TIMEOUT)
            {
                return _acquireFailure(
                    TaskDirectoryLockStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Task directory Mutex wait reached its deadline");
            }
            const auto acquired = waited == WAIT_OBJECT_0 + 1;
            const auto abandoned = waited == WAIT_ABANDONED_0 + 1;
            if (!acquired && !abandoned)
            {
                error = waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
                return _acquireFailure(
                    TaskDirectoryLockStatus::SystemError,
                    error,
                    "Task directory Mutex wait failed");
            }
            mutex.Owns = true;

            const auto cancelled = WaitForSingleObject(cancellationEvent, 0);
            if (cancelled == WAIT_OBJECT_0)
            {
                return _acquireFailure(
                    TaskDirectoryLockStatus::Cancelled,
                    ERROR_OPERATION_ABORTED,
                    "Task directory Mutex acquisition raced cancellation");
            }
            if (cancelled != WAIT_TIMEOUT)
            {
                error = cancelled == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE;
                return _acquireFailure(
                    TaskDirectoryLockStatus::SystemError,
                    error,
                    "Task directory Mutex cancellation poll failed");
            }

            DirectoryMutexAcquireResult result;
            result.Status = TaskDirectoryLockStatus::Acquired;
            result.Lease._handle = mutex.Detach();
            result.Lease._ownerThreadId = GetCurrentThreadId();
            result.Lease._owns = true;
            result.Lease._abandoned = abandoned;
            result.Lease._mutexName = mutexIdentity.MutexName;
            return result;
        }
        catch (const std::exception&)
        {
            return _acquireFailure(
                TaskDirectoryLockStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task directory Mutex acquisition exhausted local resources");
        }
        catch (...)
        {
            return _acquireFailure(
                TaskDirectoryLockStatus::ResourceFailure,
                ERROR_UNHANDLED_EXCEPTION,
                "Unexpected Task directory Mutex acquisition failure");
        }
    }
}
