// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeLaunchLockProvider.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        class RuntimeLaunchLockLease final : public IRuntimeLaunchLockLease
        {
        public:
            explicit RuntimeLaunchLockLease(RuntimeLaunchLock lock) noexcept :
                _lock{ std::move(lock) }
            {
            }

            bool Abandoned() const noexcept override
            {
                return _lock.Abandoned();
            }

        private:
            RuntimeLaunchLock _lock;
        };

        class RuntimeLaunchLockProvider final : public IRuntimeLaunchLockProvider
        {
        public:
            RuntimeLaunchLockLeaseResult Acquire(
                const RuntimeIdentity& identity,
                const RuntimeActivationDeadline deadline,
                const HANDLE cancellationEvent,
                IRuntimeActivationServices& services) noexcept override
            {
                try
                {
                    auto acquired = AcquireRuntimeLaunchLock(
                        identity,
                        deadline,
                        cancellationEvent,
                        services);
                    RuntimeLaunchLockLeaseResult result;
                    result.Status = acquired.Status;
                    result.SystemError = acquired.SystemError;
                    result.Error = std::move(acquired.Error);
                    if (acquired.Acquired())
                    {
                        result.Lease = std::make_unique<RuntimeLaunchLockLease>(
                            std::move(acquired.Lock));
                    }
                    return result;
                }
                catch (...)
                {
                    RuntimeLaunchLockLeaseResult result;
                    result.Status = RuntimeLaunchLockStatus::SystemError;
                    result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
                    result.Error = "Runtime launch-lock lease allocation failed";
                    return result;
                }
            }
        };
    }

    std::shared_ptr<IRuntimeLaunchLockProvider> MakeRuntimeLaunchLockProvider()
    {
        try
        {
            return std::make_shared<RuntimeLaunchLockProvider>();
        }
        catch (...)
        {
            return {};
        }
    }
}
