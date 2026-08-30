// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeClientConnectDisposition.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeClientLocalTests
{
    class RuntimeClientConnectDispositionTests
    {
        TEST_CLASS(RuntimeClientConnectDispositionTests);

        TEST_METHOD(ActivationFailuresOverrideGenericTransportStatus);
        TEST_METHOD(LegacyTransportClassificationRemainsStable);
    };

    void RuntimeClientConnectDispositionTests::ActivationFailuresOverrideGenericTransportStatus()
    {
        struct FailureCase
        {
            RuntimeControlConnectFailureReason Reason;
            RuntimeClientConnectDisposition Expected;
        };
        constexpr std::array cases{
            FailureCase{
                RuntimeControlConnectFailureReason::LaunchFailed,
                RuntimeClientConnectDisposition::Fatal,
            },
            FailureCase{
                RuntimeControlConnectFailureReason::ActivationWaitFailed,
                RuntimeClientConnectDisposition::Fatal,
            },
            FailureCase{
                RuntimeControlConnectFailureReason::LaunchLockTimeout,
                RuntimeClientConnectDisposition::Retry,
            },
            FailureCase{
                RuntimeControlConnectFailureReason::LaunchedProcessExited,
                RuntimeClientConnectDisposition::Retry,
            },
        };
        for (const auto& test : cases)
        {
            RuntimeControlConnectionResult result;
            result.Status = RuntimeTransportStatus::SystemError;
            result.SystemError = ERROR_GEN_FAILURE;
            result.FailureReason = test.Reason;
            VERIFY_IS_TRUE(
                EvaluateRuntimeClientConnectDisposition(result) == test.Expected);
        }
    }

    void RuntimeClientConnectDispositionTests::LegacyTransportClassificationRemainsStable()
    {
        RuntimeControlConnectionResult result;
        result.Status = RuntimeTransportStatus::ProtocolRejected;
        VERIFY_IS_TRUE(
            EvaluateRuntimeClientConnectDisposition(result) ==
            RuntimeClientConnectDisposition::Fatal);

        result.Status = RuntimeTransportStatus::SystemError;
        result.SystemError = ERROR_ACCESS_DENIED;
        VERIFY_IS_TRUE(
            EvaluateRuntimeClientConnectDisposition(result) ==
            RuntimeClientConnectDisposition::Fatal);

        result.SystemError = ERROR_NOT_READY;
        VERIFY_IS_TRUE(
            EvaluateRuntimeClientConnectDisposition(result) ==
            RuntimeClientConnectDisposition::Retry);

        result.Status = RuntimeTransportStatus::Timeout;
        VERIFY_IS_TRUE(
            EvaluateRuntimeClientConnectDisposition(result) ==
            RuntimeClientConnectDisposition::Retry);
    }
}
