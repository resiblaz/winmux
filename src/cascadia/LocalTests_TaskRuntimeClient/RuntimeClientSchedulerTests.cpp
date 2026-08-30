// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRuntimeClientScheduler.h"
#include "RuntimeClientScheduler.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeClientLocalTests
{
    class RuntimeClientSchedulerTests
    {
        TEST_CLASS(RuntimeClientSchedulerTests);

        TEST_METHOD(DeadlineEqualToNowReturnsImmediately);
        TEST_METHOD(PreSignaledWakeWinsAtDeadline);
        TEST_METHOD(FakePreSignaledWakeDoesNotConsumeInjectedFailure);
        TEST_METHOD(DeadlineAdditionSaturates);
        TEST_METHOD(InvalidWakeHandlesFailClosed);
    };

    void RuntimeClientSchedulerTests::DeadlineEqualToNowReturnsImmediately()
    {
        const auto scheduler = MakeRuntimeClientScheduler();
        VERIFY_IS_NOT_NULL(scheduler.get());
        const auto result = scheduler->WaitUntil(scheduler->Now(), {});
        VERIFY_IS_TRUE(result.Status == RuntimeClientScheduledWaitStatus::DeadlineReached);
        VERIFY_ARE_EQUAL(InvalidRuntimeClientSchedulerWakeIndex, result.WakeIndex);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_SUCCESS }, result.SystemError);
    }

    void RuntimeClientSchedulerTests::PreSignaledWakeWinsAtDeadline()
    {
        const auto scheduler = MakeRuntimeClientScheduler();
        VERIFY_IS_NOT_NULL(scheduler.get());
        const auto wake = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        VERIFY_IS_NOT_NULL(wake);
        const std::array wakeEvents{ wake };
        const auto result = scheduler->WaitUntil(scheduler->Now(), wakeEvents);
        CloseHandle(wake);

        VERIFY_IS_TRUE(result.Status == RuntimeClientScheduledWaitStatus::WakeSignaled);
        VERIFY_ARE_EQUAL(size_t{ 0 }, result.WakeIndex);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_SUCCESS }, result.SystemError);
    }

    void RuntimeClientSchedulerTests::FakePreSignaledWakeDoesNotConsumeInjectedFailure()
    {
        auto scheduler = std::make_shared<FakeRuntimeClientScheduler>();
        VERIFY_IS_TRUE(scheduler->FailNextWait(ERROR_INVALID_HANDLE));
        const auto wake = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        VERIFY_IS_NOT_NULL(wake);
        const std::array wakeEvents{ wake };
        const auto signaled = scheduler->WaitUntil(100, wakeEvents);
        VERIFY_IS_TRUE(signaled.Status == RuntimeClientScheduledWaitStatus::WakeSignaled);
        VERIFY_ARE_EQUAL(size_t{ 0 }, signaled.WakeIndex);
        ResetEvent(wake);

        const auto failed = scheduler->WaitUntil(100, {});
        VERIFY_IS_TRUE(failed.Status == RuntimeClientScheduledWaitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, failed.SystemError);
        CloseHandle(wake);
    }

    void RuntimeClientSchedulerTests::DeadlineAdditionSaturates()
    {
        constexpr auto maximum =
            (std::numeric_limits<RuntimeClientMonotonicMilliseconds>::max)();
        VERIFY_ARE_EQUAL(
            RuntimeClientMonotonicMilliseconds{ 125 },
            RuntimeClientDeadlineAfter(25, 100));
        VERIFY_ARE_EQUAL(maximum, RuntimeClientDeadlineAfter(maximum - 5, 5));
        VERIFY_ARE_EQUAL(maximum, RuntimeClientDeadlineAfter(maximum - 5, 6));
    }

    void RuntimeClientSchedulerTests::InvalidWakeHandlesFailClosed()
    {
        const auto scheduler = MakeRuntimeClientScheduler();
        VERIFY_IS_NOT_NULL(scheduler.get());

        const std::array<HANDLE, 1> nullHandle{ nullptr };
        const auto nullResult = scheduler->WaitUntil(scheduler->Now(), nullHandle);
        VERIFY_IS_TRUE(nullResult.Status == RuntimeClientScheduledWaitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_PARAMETER }, nullResult.SystemError);

        std::array<HANDLE, MaximumRuntimeClientSchedulerWakeHandles + 1> tooMany{};
        const auto countResult = scheduler->WaitUntil(scheduler->Now(), tooMany);
        VERIFY_IS_TRUE(countResult.Status == RuntimeClientScheduledWaitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_PARAMETER }, countResult.SystemError);

        const auto closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY_IS_NOT_NULL(closed);
        CloseHandle(closed);
        const std::array closedHandle{ closed };
        const auto closedResult = scheduler->WaitUntil(scheduler->Now(), closedHandle);
        VERIFY_IS_TRUE(closedResult.Status == RuntimeClientScheduledWaitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, closedResult.SystemError);
    }
}
