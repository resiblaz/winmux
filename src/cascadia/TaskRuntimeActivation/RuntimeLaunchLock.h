// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeActivationServices.h"

#include "RuntimeIdentity.h"

#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeLaunchLockStatus
    {
        Acquired,
        Cancelled,
        TimedOut,
        InvalidIdentity,
        InvalidDeadline,
        SecurityFailure,
        SystemError,
    };

    enum class RuntimeLaunchLockReleaseStatus
    {
        Released,
        NotOwned,
        WrongThread,
        SystemError,
    };

    struct RuntimeLaunchLockResult;

    // Move-only and thread-affine while owned. Release and ordinary RAII
    // destruction must occur on the thread that acquired the mutex.
    class RuntimeLaunchLock final
    {
    public:
        RuntimeLaunchLock() noexcept = default;
        ~RuntimeLaunchLock();

        RuntimeLaunchLock(const RuntimeLaunchLock&) = delete;
        RuntimeLaunchLock& operator=(const RuntimeLaunchLock&) = delete;
        RuntimeLaunchLock(RuntimeLaunchLock&& other) noexcept;
        RuntimeLaunchLock& operator=(RuntimeLaunchLock&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] bool Abandoned() const noexcept;
        [[nodiscard]] DWORD OwnerThreadId() const noexcept;
        // Borrowed for diagnostics/tests; never inherit or close this handle.
        [[nodiscard]] HANDLE Handle() const noexcept;
        [[nodiscard]] RuntimeLaunchLockReleaseStatus Release() noexcept;

    private:
        friend struct RuntimeLaunchLockResult;
        friend RuntimeLaunchLockResult AcquireRuntimeLaunchLock(
            const RuntimeIdentity&,
            RuntimeActivationDeadline,
            HANDLE,
            IRuntimeActivationServices&);

        void _close() noexcept;

        HANDLE _handle{};
        DWORD _ownerThreadId{};
        bool _owns{};
        bool _abandoned{};
    };

    struct RuntimeLaunchLockResult
    {
        RuntimeLaunchLockStatus Status{ RuntimeLaunchLockStatus::SystemError };
        RuntimeLaunchLock Lock;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Acquired() const noexcept
        {
            return Status == RuntimeLaunchLockStatus::Acquired && static_cast<bool>(Lock);
        }
    };

    [[nodiscard]] RuntimeLaunchLockResult AcquireRuntimeLaunchLock(
        const RuntimeIdentity& identity,
        RuntimeActivationDeadline deadline,
        HANDLE cancellationEvent,
        IRuntimeActivationServices& services);

    [[nodiscard]] RuntimeLaunchLockResult AcquireRuntimeLaunchLock(
        const RuntimeIdentity& identity,
        RuntimeActivationDeadline deadline,
        HANDLE cancellationEvent = nullptr);
}
