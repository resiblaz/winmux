// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeClientInterfaces.h"

#include "ProtocolValidation.h"

#include <unordered_set>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeClientLocalTests
{
    class RuntimeClientInterfaceTests
    {
        TEST_CLASS(RuntimeClientInterfaceTests);

        TEST_METHOD(DefaultBackoffIsBoundedAndDeterministic);
        TEST_METHOD(DefaultRequestIdsAreCanonicalAndUnique);
    };

    void RuntimeClientInterfaceTests::DefaultBackoffIsBoundedAndDeterministic()
    {
        const auto policy = MakeRuntimeClientBackoffPolicy();
        VERIFY_IS_NOT_NULL(policy.get());
        constexpr std::array<uint32_t, 10> expected{
            0, 100, 200, 400, 800, 1600, 3200, 5000, 5000, 5000
        };
        for (size_t failure = 0; failure < expected.size(); ++failure)
        {
            VERIFY_ARE_EQUAL(
                expected[failure],
                policy->DelayForFailure(static_cast<uint32_t>(failure)));
        }
        VERIFY_ARE_EQUAL(uint32_t{ 5000 }, policy->DelayForFailure(UINT32_MAX));
    }

    void RuntimeClientInterfaceTests::DefaultRequestIdsAreCanonicalAndUnique()
    {
        const auto source = MakeRuntimeClientRequestIdSource();
        VERIFY_IS_NOT_NULL(source.get());
        std::unordered_set<std::string> observed;
        for (size_t index = 0; index < 100; ++index)
        {
            const auto requestId = source->Next();
            VERIFY_IS_TRUE(requestId.has_value());
            VERIFY_IS_TRUE(IsCanonicalProtocolUuid(*requestId));
            VERIFY_IS_TRUE(observed.emplace(*requestId).second);
        }
    }
}
