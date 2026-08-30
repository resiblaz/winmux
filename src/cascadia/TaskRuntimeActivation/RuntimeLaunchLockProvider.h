// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeActivationServices.h"
#include "RuntimeLaunchLock.h"

#include "RuntimeIdentity.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    class IRuntimeLaunchLockLease
    {
    public:
        virtual ~IRuntimeLaunchLockLease() = default;
        [[nodiscard]] virtual bool Abandoned() const noexcept = 0;
    };

    struct RuntimeLaunchLockLeaseResult
    {
        RuntimeLaunchLockStatus Status{ RuntimeLaunchLockStatus::SystemError };
        std::unique_ptr<IRuntimeLaunchLockLease> Lease;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Acquired() const noexcept
        {
            return Status == RuntimeLaunchLockStatus::Acquired && Lease;
        }
    };

    class IRuntimeLaunchLockProvider
    {
    public:
        virtual ~IRuntimeLaunchLockProvider() = default;
        [[nodiscard]] virtual RuntimeLaunchLockLeaseResult Acquire(
            const RuntimeIdentity& identity,
            RuntimeActivationDeadline deadline,
            HANDLE cancellationEvent,
            IRuntimeActivationServices& services) noexcept = 0;
    };

    [[nodiscard]] std::shared_ptr<IRuntimeLaunchLockProvider>
        MakeRuntimeLaunchLockProvider();
}
