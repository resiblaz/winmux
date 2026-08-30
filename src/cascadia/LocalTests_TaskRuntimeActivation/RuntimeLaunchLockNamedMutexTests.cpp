// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ActivationTestSupport.h"
#include "RuntimeLaunchLock.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    namespace
    {
        class EnteringRuntimeActivationServices final : public IRuntimeActivationServices
        {
        public:
            EnteringRuntimeActivationServices(
                std::shared_ptr<IRuntimeActivationServices> inner,
                HANDLE entered,
                HANDLE signalOnEntry = nullptr) noexcept :
                _inner{ std::move(inner) },
                _entered{ entered },
                _signalOnEntry{ signalOnEntry }
            {
            }

            RuntimeActivationMonotonicMilliseconds Now() noexcept override
            {
                return _inner->Now();
            }

            RuntimeActivationWaitResult WaitUntil(
                const RuntimeActivationDeadline deadline,
                const std::span<const HANDLE> wakeHandles) noexcept override
            {
                SetEvent(_entered);
                if (_signalOnEntry)
                {
                    SetEvent(_signalOnEntry);
                }
                return _inner->WaitUntil(deadline, wakeHandles);
            }

        private:
            std::shared_ptr<IRuntimeActivationServices> _inner;
            HANDLE _entered{};
            HANDLE _signalOnEntry{};
        };
    }

    class RuntimeLaunchLockNamedMutexTests
    {
        TEST_CLASS(RuntimeLaunchLockNamedMutexTests);

        TEST_METHOD(SameIdentitySerializesCompetingThreads);
        TEST_METHOD(CancellationWinsBeforeAndDuringMutexWait);
        TEST_METHOD(AbsoluteDeadlineBoundsContendedWait);
        TEST_METHOD(DifferentRuntimeIdentitiesRemainIndependent);
    };

    void RuntimeLaunchLockNamedMutexTests::SameIdentitySerializesCompetingThreads()
    {
        UniqueActivationIdentity identity;
        const auto services = MakeRuntimeActivationWindowsServices();
        auto first = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(first.Acquired());

        auto entered = MakeTestEvent();
        auto completed = MakeTestEvent();
        RuntimeLaunchLockStatus workerStatus{ RuntimeLaunchLockStatus::SystemError };
        RuntimeLaunchLockReleaseStatus workerRelease{ RuntimeLaunchLockReleaseStatus::NotOwned };
        std::thread competitor{ [&] {
            SetEvent(entered.get());
            auto acquired = AcquireRuntimeLaunchLock(
                identity.Identity(),
                DeadlineAfter(*services),
                nullptr,
                *services);
            workerStatus = acquired.Status;
            if (acquired.Acquired())
            {
                workerRelease = acquired.Lock.Release();
            }
            SetEvent(completed.get());
        } };

        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(entered.get(), 1000));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_TIMEOUT }, WaitForSingleObject(completed.get(), 50));
        VERIFY_IS_TRUE(first.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(completed.get(), 2000));
        competitor.join();
        VERIFY_IS_TRUE(workerStatus == RuntimeLaunchLockStatus::Acquired);
        VERIFY_IS_TRUE(workerRelease == RuntimeLaunchLockReleaseStatus::Released);
    }

    void RuntimeLaunchLockNamedMutexTests::CancellationWinsBeforeAndDuringMutexWait()
    {
        UniqueActivationIdentity identity;
        const auto services = MakeRuntimeActivationWindowsServices();
        auto preCancelled = MakeTestEvent(true, true);
        const auto immediate = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            preCancelled.get(),
            *services);
        VERIFY_IS_TRUE(immediate.Status == RuntimeLaunchLockStatus::Cancelled);

        auto owner = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(owner.Acquired());
        auto cancellation = MakeTestEvent();
        auto waitEntered = MakeTestEvent();
        EnteringRuntimeActivationServices enteringServices{
            services,
            waitEntered.get(),
            cancellation.get(),
        };
        RuntimeLaunchLockStatus workerStatus{ RuntimeLaunchLockStatus::SystemError };
        std::thread waiter{ [&] {
            const auto result = AcquireRuntimeLaunchLock(
                identity.Identity(),
                DeadlineAfter(enteringServices),
                cancellation.get(),
                enteringServices);
            workerStatus = result.Status;
        } };
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(waitEntered.get(), 1000));
        waiter.join();
        VERIFY_IS_TRUE(workerStatus == RuntimeLaunchLockStatus::Cancelled);
        VERIFY_IS_TRUE(owner.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
    }

    void RuntimeLaunchLockNamedMutexTests::AbsoluteDeadlineBoundsContendedWait()
    {
        UniqueActivationIdentity identity;
        const auto services = MakeRuntimeActivationWindowsServices();
        auto owner = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(owner.Acquired());

        RuntimeLaunchLockStatus workerStatus{ RuntimeLaunchLockStatus::SystemError };
        DWORD workerError{};
        auto waitEntered = MakeTestEvent();
        EnteringRuntimeActivationServices enteringServices{
            services,
            waitEntered.get(),
        };
        std::thread waiter{ [&] {
            const auto deadline = MakeRuntimeActivationDeadline(enteringServices.Now(), 200);
            const auto result = AcquireRuntimeLaunchLock(
                identity.Identity(),
                deadline.value_or(RuntimeActivationDeadline{}),
                nullptr,
                enteringServices);
            workerStatus = result.Status;
            workerError = result.SystemError;
        } };
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(waitEntered.get(), 1000));
        const auto started = GetTickCount64();
        waiter.join();
        const auto elapsed = GetTickCount64() - started;
        VERIFY_IS_TRUE(workerStatus == RuntimeLaunchLockStatus::TimedOut);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_TIMEOUT }, workerError);
        VERIFY_IS_TRUE(elapsed < 2000);
        VERIFY_IS_TRUE(owner.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
    }

    void RuntimeLaunchLockNamedMutexTests::DifferentRuntimeIdentitiesRemainIndependent()
    {
        UniqueActivationIdentity firstIdentity;
        UniqueActivationIdentity secondIdentity;
        const auto services = MakeRuntimeActivationWindowsServices();
        auto first = AcquireRuntimeLaunchLock(
            firstIdentity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        auto second = AcquireRuntimeLaunchLock(
            secondIdentity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(first.Acquired());
        VERIFY_IS_TRUE(second.Acquired());
        VERIFY_ARE_NOT_EQUAL(
            firstIdentity.Identity().LaunchMutexName,
            secondIdentity.Identity().LaunchMutexName);
        VERIFY_IS_TRUE(second.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
        VERIFY_IS_TRUE(first.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
    }
}
