// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeLaunchLock.h"
#include "RuntimeLaunchLockSecurity.h"

#include "RuntimeSecurity.h"

#include <exception>

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        RuntimeLaunchLockResult _failure(
            const RuntimeLaunchLockStatus status,
            const DWORD systemError,
            std::string error)
        {
            RuntimeLaunchLockResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _matchesCurrentUserIdentity(const RuntimeIdentity& identity) noexcept
        {
            try
            {
                const auto rebuilt = BuildCurrentUserRuntimeIdentity(
                    identity.Channel,
                    identity.SettingsRoot);
                return rebuilt.Succeeded() &&
                       rebuilt.Identity.UserSid == identity.UserSid &&
                       rebuilt.Identity.ProtocolIdentity == identity.ProtocolIdentity &&
                       rebuilt.Identity.LaunchMutexName == identity.LaunchMutexName;
            }
            catch (...)
            {
                return false;
            }
        }

        void _releaseAndClose(const HANDLE handle) noexcept
        {
            if (handle && handle != INVALID_HANDLE_VALUE)
            {
                (void)ReleaseMutex(handle);
                CloseHandle(handle);
            }
        }
    }

    RuntimeLaunchLock::~RuntimeLaunchLock()
    {
        _close();
    }

    RuntimeLaunchLock::RuntimeLaunchLock(RuntimeLaunchLock&& other) noexcept :
        _handle{},
        _ownerThreadId{},
        _owns{},
        _abandoned{}
    {
        if (other._owns && GetCurrentThreadId() != other._ownerThreadId)
        {
            std::terminate();
        }
        _handle = std::exchange(other._handle, nullptr);
        _ownerThreadId = std::exchange(other._ownerThreadId, 0);
        _owns = std::exchange(other._owns, false);
        _abandoned = std::exchange(other._abandoned, false);
    }

    RuntimeLaunchLock& RuntimeLaunchLock::operator=(RuntimeLaunchLock&& other) noexcept
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
        }
        return *this;
    }

    RuntimeLaunchLock::operator bool() const noexcept
    {
        return _owns && _handle && _handle != INVALID_HANDLE_VALUE;
    }

    bool RuntimeLaunchLock::Abandoned() const noexcept
    {
        return static_cast<bool>(*this) && _abandoned;
    }

    DWORD RuntimeLaunchLock::OwnerThreadId() const noexcept
    {
        return static_cast<bool>(*this) ? _ownerThreadId : 0;
    }

    HANDLE RuntimeLaunchLock::Handle() const noexcept
    {
        return static_cast<bool>(*this) ? _handle : nullptr;
    }

    RuntimeLaunchLockReleaseStatus RuntimeLaunchLock::Release() noexcept
    {
        if (!static_cast<bool>(*this))
        {
            return RuntimeLaunchLockReleaseStatus::NotOwned;
        }
        if (GetCurrentThreadId() != _ownerThreadId)
        {
            return RuntimeLaunchLockReleaseStatus::WrongThread;
        }
        if (!ReleaseMutex(_handle))
        {
            return RuntimeLaunchLockReleaseStatus::SystemError;
        }

        CloseHandle(_handle);
        _handle = nullptr;
        _ownerThreadId = 0;
        _owns = false;
        _abandoned = false;
        return RuntimeLaunchLockReleaseStatus::Released;
    }

    void RuntimeLaunchLock::_close() noexcept
    {
        if (!_handle || _handle == INVALID_HANDLE_VALUE)
        {
            return;
        }
        if (_owns && GetCurrentThreadId() != _ownerThreadId)
        {
            // A Windows mutex can only be released by its owner thread.
            // Silently closing here would strand ownership until that thread
            // exits and corrupt cross-process activation coordination.
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
    }

    RuntimeLaunchLockResult AcquireRuntimeLaunchLock(
        const RuntimeIdentity& identity,
        const RuntimeActivationDeadline deadline,
        const HANDLE cancellationEvent,
        IRuntimeActivationServices& services)
    {
        try
        {
            if (!deadline)
            {
                return _failure(
                    RuntimeLaunchLockStatus::InvalidDeadline,
                    ERROR_INVALID_PARAMETER,
                    "Runtime launch-lock deadline is invalid");
            }
            if (!_matchesCurrentUserIdentity(identity))
            {
                return _failure(
                    RuntimeLaunchLockStatus::InvalidIdentity,
                    ERROR_INVALID_PARAMETER,
                    "Runtime launch-lock identity does not match the current user");
            }

            auto security = CreateRuntimeSecurityAttributes(identity.UserSid);
            if (!security.Succeeded())
            {
                return _failure(
                    RuntimeLaunchLockStatus::SecurityFailure,
                    security.Status == RuntimeSecurityStatus::InvalidUserSid ?
                        ERROR_INVALID_SID :
                        ERROR_ACCESS_DENIED,
                    std::move(security.Error));
            }

            SetLastError(ERROR_SUCCESS);
            const auto mutex = CreateMutexW(
                const_cast<SECURITY_ATTRIBUTES*>(security.Security.Attributes()),
                FALSE,
                identity.LaunchMutexName.c_str());
            if (!mutex)
            {
                const auto error = GetLastError();
                return _failure(
                    error == ERROR_ACCESS_DENIED ?
                        RuntimeLaunchLockStatus::SecurityFailure :
                        RuntimeLaunchLockStatus::SystemError,
                    error,
                    "CreateMutex failed for the Runtime launch lock");
            }
            if (!SetHandleInformation(mutex, HANDLE_FLAG_INHERIT, 0))
            {
                const auto error = GetLastError();
                CloseHandle(mutex);
                return _failure(
                    RuntimeLaunchLockStatus::SystemError,
                    error,
                    "Runtime launch-lock handle could not be made non-inheritable");
            }
            const auto validated = details::ValidateRuntimeLaunchLockSecurity(
                mutex,
                identity.UserSid);
            if (!validated.Succeeded)
            {
                CloseHandle(mutex);
                return _failure(
                    RuntimeLaunchLockStatus::SecurityFailure,
                    validated.SystemError,
                    std::move(validated.Error));
            }

            std::array<HANDLE, 2> handles{};
            size_t count{};
            std::optional<size_t> cancellationIndex;
            if (cancellationEvent)
            {
                cancellationIndex = count;
                handles[count++] = cancellationEvent;
            }
            const auto mutexIndex = count;
            handles[count++] = mutex;

            const auto waited = services.WaitUntil(
                deadline,
                std::span<const HANDLE>{ handles.data(), count });
            if (waited.Status == RuntimeActivationWaitStatus::DeadlineReached)
            {
                CloseHandle(mutex);
                return _failure(
                    RuntimeLaunchLockStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Runtime launch-lock deadline expired");
            }
            if (waited.Status == RuntimeActivationWaitStatus::Failed)
            {
                CloseHandle(mutex);
                return _failure(
                    RuntimeLaunchLockStatus::SystemError,
                    waited.SystemError,
                    "Runtime launch-lock wait failed");
            }
            if (cancellationIndex && waited.WakeIndex == *cancellationIndex)
            {
                CloseHandle(mutex);
                return _failure(
                    RuntimeLaunchLockStatus::Cancelled,
                    ERROR_OPERATION_ABORTED,
                    "Runtime launch-lock wait was cancelled");
            }
            if (waited.WakeIndex != mutexIndex ||
                (waited.Status != RuntimeActivationWaitStatus::WakeSignaled &&
                 waited.Status != RuntimeActivationWaitStatus::WakeAbandoned))
            {
                CloseHandle(mutex);
                return _failure(
                    RuntimeLaunchLockStatus::SystemError,
                    ERROR_INVALID_DATA,
                    "Runtime launch-lock wait returned an invalid wake result");
            }

            if (cancellationEvent)
            {
                const auto cancelled = WaitForSingleObject(cancellationEvent, 0);
                if (cancelled == WAIT_OBJECT_0)
                {
                    _releaseAndClose(mutex);
                    return _failure(
                        RuntimeLaunchLockStatus::Cancelled,
                        ERROR_OPERATION_ABORTED,
                        "Runtime launch-lock acquisition raced cancellation");
                }
                if (cancelled != WAIT_TIMEOUT)
                {
                    const auto error = cancelled == WAIT_FAILED ?
                                           GetLastError() :
                                           ERROR_INVALID_HANDLE;
                    _releaseAndClose(mutex);
                    return _failure(
                        RuntimeLaunchLockStatus::SystemError,
                        error,
                        "Runtime launch-lock cancellation poll failed");
                }
            }

            RuntimeLaunchLockResult result;
            result.Status = RuntimeLaunchLockStatus::Acquired;
            result.Lock._handle = mutex;
            result.Lock._ownerThreadId = GetCurrentThreadId();
            result.Lock._owns = true;
            result.Lock._abandoned =
                waited.Status == RuntimeActivationWaitStatus::WakeAbandoned;
            return result;
        }
        catch (const std::exception& exception)
        {
            return _failure(
                RuntimeLaunchLockStatus::SystemError,
                ERROR_NOT_ENOUGH_MEMORY,
                exception.what());
        }
        catch (...)
        {
            return _failure(
                RuntimeLaunchLockStatus::SystemError,
                ERROR_UNHANDLED_EXCEPTION,
                "Unexpected Runtime launch-lock failure");
        }
    }

    RuntimeLaunchLockResult AcquireRuntimeLaunchLock(
        const RuntimeIdentity& identity,
        const RuntimeActivationDeadline deadline,
        const HANDLE cancellationEvent)
    {
        const auto services = MakeRuntimeActivationWindowsServices();
        if (!services)
        {
            return _failure(
                RuntimeLaunchLockStatus::SystemError,
                ERROR_NOT_ENOUGH_MEMORY,
                "Runtime activation services are unavailable");
        }
        return AcquireRuntimeLaunchLock(identity, deadline, cancellationEvent, *services);
    }
}
