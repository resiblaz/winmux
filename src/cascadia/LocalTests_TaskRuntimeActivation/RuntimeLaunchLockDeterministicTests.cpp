// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ActivationTestSupport.h"
#include "FakeRuntimeActivationServices.h"
#include "RuntimeLaunchLock.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeLaunchLockDeterministicTests
    {
        TEST_CLASS(RuntimeLaunchLockDeterministicTests);

        TEST_METHOD(PassesOneUnchangedAbsoluteDeadlineToWaitSeam);
        TEST_METHOD(CancellationHandleIsAlwaysHighestPriority);
        TEST_METHOD(InvalidIdentityDeadlineAndWaitFailureFailClosed);
    };

    void RuntimeLaunchLockDeterministicTests::PassesOneUnchangedAbsoluteDeadlineToWaitSeam()
    {
        UniqueActivationIdentity identity;
        FakeRuntimeActivationServices services{ 100 };
        const RuntimeActivationDeadline deadline{ 725 };
        services.Enqueue({
            RuntimeActivationWaitStatus::DeadlineReached,
            InvalidRuntimeActivationWaitIndex,
            ERROR_SUCCESS,
        });

        const auto result = AcquireRuntimeLaunchLock(
            identity.Identity(),
            deadline,
            nullptr,
            services);
        VERIFY_IS_TRUE(result.Status == RuntimeLaunchLockStatus::TimedOut);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_TIMEOUT }, result.SystemError);
        const auto deadlines = services.Deadlines();
        VERIFY_ARE_EQUAL(size_t{ 1 }, deadlines.size());
        if (!deadlines.empty())
        {
            VERIFY_ARE_EQUAL(deadline.Value, deadlines.front().Value);
        }
        const auto handles = services.WakeHandles();
        VERIFY_ARE_EQUAL(size_t{ 1 }, handles.size());
        VERIFY_ARE_EQUAL(size_t{ 1 }, handles.empty() ? 0 : handles.front().size());
    }

    void RuntimeLaunchLockDeterministicTests::CancellationHandleIsAlwaysHighestPriority()
    {
        UniqueActivationIdentity identity;
        FakeRuntimeActivationServices services{ 100 };
        auto cancellation = MakeTestEvent(true, true);
        services.Enqueue({
            RuntimeActivationWaitStatus::WakeSignaled,
            0,
            ERROR_SUCCESS,
        });

        const auto result = AcquireRuntimeLaunchLock(
            identity.Identity(),
            RuntimeActivationDeadline{ 500 },
            cancellation.get(),
            services);
        VERIFY_IS_TRUE(result.Status == RuntimeLaunchLockStatus::Cancelled);
        VERIFY_IS_FALSE(result.Acquired());
        const auto handles = services.WakeHandles();
        VERIFY_ARE_EQUAL(size_t{ 1 }, handles.size());
        if (!handles.empty())
        {
            VERIFY_ARE_EQUAL(size_t{ 2 }, handles.front().size());
            if (handles.front().size() == 2)
            {
                VERIFY_ARE_EQUAL(cancellation.get(), handles.front()[0]);
                VERIFY_IS_NOT_NULL(handles.front()[1]);
            }
        }

        FakeRuntimeActivationServices mutexWakeServices{ 100 };
        mutexWakeServices.Enqueue({
            RuntimeActivationWaitStatus::WakeSignaled,
            1,
            ERROR_SUCCESS,
        });
        const auto raced = AcquireRuntimeLaunchLock(
            identity.Identity(),
            RuntimeActivationDeadline{ 500 },
            cancellation.get(),
            mutexWakeServices);
        VERIFY_IS_TRUE(raced.Status == RuntimeLaunchLockStatus::Cancelled);
        VERIFY_IS_FALSE(raced.Acquired());
    }

    void RuntimeLaunchLockDeterministicTests::InvalidIdentityDeadlineAndWaitFailureFailClosed()
    {
        UniqueActivationIdentity identity;
        FakeRuntimeActivationServices services{ 100 };
        const auto invalidDeadline = AcquireRuntimeLaunchLock(
            identity.Identity(),
            {},
            nullptr,
            services);
        VERIFY_IS_TRUE(invalidDeadline.Status == RuntimeLaunchLockStatus::InvalidDeadline);

        auto invalidIdentity = identity.Identity();
        invalidIdentity.LaunchMutexName.append(L".wrong");
        const auto rejectedIdentity = AcquireRuntimeLaunchLock(
            invalidIdentity,
            RuntimeActivationDeadline{ 500 },
            nullptr,
            services);
        VERIFY_IS_TRUE(rejectedIdentity.Status == RuntimeLaunchLockStatus::InvalidIdentity);

        services.Enqueue({
            RuntimeActivationWaitStatus::Failed,
            InvalidRuntimeActivationWaitIndex,
            ERROR_INVALID_HANDLE,
        });
        const auto failed = AcquireRuntimeLaunchLock(
            identity.Identity(),
            RuntimeActivationDeadline{ 500 },
            nullptr,
            services);
        VERIFY_IS_TRUE(failed.Status == RuntimeLaunchLockStatus::SystemError);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, failed.SystemError);
        VERIFY_ARE_EQUAL(size_t{ 1 }, services.Deadlines().size());
    }
}
