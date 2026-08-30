// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ActivationTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeActivationWindowsServicesTests
    {
        TEST_CLASS(RuntimeActivationWindowsServicesTests);

        TEST_METHOD(DeadlineAndPriorityOrderingAreExact);
        TEST_METHOD(WaitWakesForSignalBeforeDeadline);
        TEST_METHOD(InvalidHandlesFailClosed);
    };

    void RuntimeActivationWindowsServicesTests::DeadlineAndPriorityOrderingAreExact()
    {
        const auto services = MakeRuntimeActivationWindowsServices();
        VERIFY_IS_NOT_NULL(services.get());
        auto first = MakeTestEvent(true, true);
        auto second = MakeTestEvent(true, true);
        const std::array handles{ first.get(), second.get() };
        const RuntimeActivationDeadline deadline{ services->Now() };
        const auto result = services->WaitUntil(deadline, handles);
        VERIFY_IS_TRUE(result.Status == RuntimeActivationWaitStatus::WakeSignaled);
        VERIFY_ARE_EQUAL(size_t{ 0 }, result.WakeIndex);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_SUCCESS }, result.SystemError);

        ResetEvent(first.get());
        ResetEvent(second.get());
        const auto expired = services->WaitUntil(deadline, handles);
        VERIFY_IS_TRUE(expired.Status == RuntimeActivationWaitStatus::DeadlineReached);
        VERIFY_ARE_EQUAL(InvalidRuntimeActivationWaitIndex, expired.WakeIndex);
    }

    void RuntimeActivationWindowsServicesTests::WaitWakesForSignalBeforeDeadline()
    {
        const auto services = MakeRuntimeActivationWindowsServices();
        auto wake = MakeTestEvent();
        const auto deadline = DeadlineAfter(*services, 2000);
        std::thread signaler{ [&] {
            Sleep(20);
            SetEvent(wake.get());
        } };
        const std::array handles{ wake.get() };
        const auto result = services->WaitUntil(deadline, handles);
        signaler.join();
        VERIFY_IS_TRUE(result.Status == RuntimeActivationWaitStatus::WakeSignaled);
        VERIFY_ARE_EQUAL(size_t{ 0 }, result.WakeIndex);
    }

    void RuntimeActivationWindowsServicesTests::InvalidHandlesFailClosed()
    {
        const auto services = MakeRuntimeActivationWindowsServices();
        const auto deadline = DeadlineAfter(*services);
        const auto invalidDeadline = services->WaitUntil({}, {});
        VERIFY_IS_TRUE(invalidDeadline.Status == RuntimeActivationWaitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_PARAMETER }, invalidDeadline.SystemError);

        const std::array<HANDLE, 1> nullHandle{ nullptr };
        const auto nullResult = services->WaitUntil(deadline, nullHandle);
        VERIFY_IS_TRUE(nullResult.Status == RuntimeActivationWaitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_PARAMETER }, nullResult.SystemError);

        std::array<HANDLE, MaximumRuntimeActivationWaitHandles + 1> tooMany{};
        const auto countResult = services->WaitUntil(deadline, tooMany);
        VERIFY_IS_TRUE(countResult.Status == RuntimeActivationWaitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_PARAMETER }, countResult.SystemError);
    }
}
