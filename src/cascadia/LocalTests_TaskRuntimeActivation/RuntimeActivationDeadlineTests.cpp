// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeActivationDeadline.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeActivationDeadlineTests
    {
        TEST_CLASS(RuntimeActivationDeadlineTests);

        TEST_METHOD(DeadlineUsesOneAbsoluteMonotonicValue);
        TEST_METHOD(DeadlineSaturatesWithoutWrapping);
        TEST_METHOD(ZeroDelayAndExhaustedClockFailClosed);
    };

    void RuntimeActivationDeadlineTests::DeadlineUsesOneAbsoluteMonotonicValue()
    {
        const auto deadline = MakeRuntimeActivationDeadline(125, 75);
        VERIFY_IS_TRUE(deadline.has_value());
        VERIFY_ARE_EQUAL(RuntimeActivationMonotonicMilliseconds{ 200 }, deadline ? deadline->Value : 0);
        VERIFY_IS_FALSE(RuntimeActivationDeadlineReached(*deadline, 199));
        VERIFY_IS_TRUE(RuntimeActivationDeadlineReached(*deadline, 200));
        VERIFY_ARE_EQUAL(
            RuntimeActivationMonotonicMilliseconds{ 75 },
            RuntimeActivationRemainingMilliseconds(*deadline, 125));
        VERIFY_ARE_EQUAL(
            RuntimeActivationMonotonicMilliseconds{ 1 },
            RuntimeActivationRemainingMilliseconds(*deadline, 199));
        VERIFY_ARE_EQUAL(
            RuntimeActivationMonotonicMilliseconds{ 0 },
            RuntimeActivationRemainingMilliseconds(*deadline, 200));
    }

    void RuntimeActivationDeadlineTests::DeadlineSaturatesWithoutWrapping()
    {
        constexpr auto maximum =
            (std::numeric_limits<RuntimeActivationMonotonicMilliseconds>::max)();
        const auto exact = MakeRuntimeActivationDeadline(maximum - 100, 100);
        const auto saturated = MakeRuntimeActivationDeadline(maximum - 50, 100);
        VERIFY_IS_TRUE(exact.has_value());
        VERIFY_IS_TRUE(saturated.has_value());
        VERIFY_ARE_EQUAL(maximum, exact ? exact->Value : 0);
        VERIFY_ARE_EQUAL(maximum, saturated ? saturated->Value : 0);
        VERIFY_IS_FALSE(RuntimeActivationDeadlineReached(*saturated, maximum - 1));
        VERIFY_IS_TRUE(RuntimeActivationDeadlineReached(*saturated, maximum));
    }

    void RuntimeActivationDeadlineTests::ZeroDelayAndExhaustedClockFailClosed()
    {
        constexpr auto maximum =
            (std::numeric_limits<RuntimeActivationMonotonicMilliseconds>::max)();
        VERIFY_IS_FALSE(MakeRuntimeActivationDeadline(10, 0).has_value());
        VERIFY_IS_FALSE(MakeRuntimeActivationDeadline(maximum, 1).has_value());
        VERIFY_IS_TRUE(RuntimeActivationDeadlineReached({}, 0));
        VERIFY_ARE_EQUAL(
            RuntimeActivationMonotonicMilliseconds{ 0 },
            RuntimeActivationRemainingMilliseconds({}, 0));
    }
}
