// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskIdentifiers.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;

namespace TaskCoreLocalTests
{
    class TaskCoreIdentifierTests
    {
        TEST_CLASS(TaskCoreIdentifierTests);

        TEST_METHOD(AcceptsCanonicalNonZeroUuid);
        TEST_METHOD(RejectsNoncanonicalAndZeroUuid);
        TEST_METHOD(StrongIdsRemainTypeSafeValues);
    };

    void TaskCoreIdentifierTests::AcceptsCanonicalNonZeroUuid()
    {
        constexpr std::string_view value{ "12345678-1234-4abc-8def-1234567890ab" };
        VERIFY_IS_TRUE(IsCanonicalUuid(value));
        const auto parsed = ParseId<TaskId>(value);
        VERIFY_IS_TRUE(parsed.has_value());
        VERIFY_ARE_EQUAL(std::string{ value }, parsed->Value());
    }

    void TaskCoreIdentifierTests::RejectsNoncanonicalAndZeroUuid()
    {
        VERIFY_IS_FALSE(IsCanonicalUuid("12345678-1234-4ABC-8def-1234567890ab"));
        VERIFY_IS_FALSE(IsCanonicalUuid("{12345678-1234-4abc-8def-1234567890ab}"));
        VERIFY_IS_FALSE(IsCanonicalUuid("00000000-0000-0000-0000-000000000000"));
        VERIFY_IS_FALSE(ParseId<TaskRunId>("not-a-uuid").has_value());
    }

    void TaskCoreIdentifierTests::StrongIdsRemainTypeSafeValues()
    {
        const TaskId first{ "12345678-1234-4abc-8def-1234567890ab" };
        const TaskId same{ "12345678-1234-4abc-8def-1234567890ab" };
        const TaskId different{ "22345678-1234-4abc-8def-1234567890ab" };
        VERIFY_IS_TRUE(first == same);
        VERIFY_IS_TRUE(first != different);
        VERIFY_IS_TRUE(static_cast<bool>(first));
        VERIFY_IS_FALSE(static_cast<bool>(TaskId{}));
    }
}
