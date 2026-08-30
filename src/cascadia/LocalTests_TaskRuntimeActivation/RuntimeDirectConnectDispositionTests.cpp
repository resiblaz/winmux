// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRuntimeLaunchingConnector.h"
#include "RuntimeDirectConnectDisposition.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskRuntimeActivationLocalTests;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeDirectConnectDispositionTests
    {
        TEST_CLASS(RuntimeDirectConnectDispositionTests);

        TEST_METHOD(OnlyStrictInitialAbsenceMayLaunch);
        TEST_METHOD(RetryCancelledAndFatalReasonsAreExhaustive);
    };

    void RuntimeDirectConnectDispositionTests::OnlyStrictInitialAbsenceMayLaunch()
    {
        auto connected = ConnectedResult();
        VERIFY_IS_TRUE(EvaluateRuntimeDirectConnectDisposition(connected) ==
                       RuntimeDirectConnectDisposition::Connected);

        auto absent = AbsentResult();
        VERIFY_IS_TRUE(EvaluateRuntimeDirectConnectDisposition(absent) ==
                       RuntimeDirectConnectDisposition::MayLaunch);
        absent.Phase = RuntimeControlConnectPhase::Handshaking;
        VERIFY_IS_TRUE(EvaluateRuntimeDirectConnectDisposition(absent) ==
                       RuntimeDirectConnectDisposition::Fatal);
        absent.Phase = RuntimeControlConnectPhase::Connecting;
        absent.SystemError = ERROR_PIPE_BUSY;
        VERIFY_IS_TRUE(EvaluateRuntimeDirectConnectDisposition(absent) ==
                       RuntimeDirectConnectDisposition::Fatal);
        absent.SystemError = ERROR_FILE_NOT_FOUND;
        absent.Connection = std::make_unique<FakeConnectedControlConnection>();
        VERIFY_IS_TRUE(EvaluateRuntimeDirectConnectDisposition(absent) ==
                       RuntimeDirectConnectDisposition::Fatal);
    }

    void RuntimeDirectConnectDispositionTests::RetryCancelledAndFatalReasonsAreExhaustive()
    {
        struct ReasonCase
        {
            RuntimeControlConnectFailureReason Reason;
            RuntimeDirectConnectDisposition Expected;
        };
        constexpr std::array<ReasonCase, 18> cases{
            ReasonCase{ RuntimeControlConnectFailureReason::None, RuntimeDirectConnectDisposition::Fatal },
            ReasonCase{
                RuntimeControlConnectFailureReason::InvalidConfiguration,
                RuntimeDirectConnectDisposition::Fatal,
            },
            ReasonCase{ RuntimeControlConnectFailureReason::Cancelled, RuntimeDirectConnectDisposition::Cancelled },
            ReasonCase{
                RuntimeControlConnectFailureReason::DeadlineExpired,
                RuntimeDirectConnectDisposition::RetryConnectOnly,
            },
            ReasonCase{ RuntimeControlConnectFailureReason::EndpointAbsent, RuntimeDirectConnectDisposition::Fatal },
            ReasonCase{
                RuntimeControlConnectFailureReason::EndpointBusy,
                RuntimeDirectConnectDisposition::RetryConnectOnly,
            },
            ReasonCase{
                RuntimeControlConnectFailureReason::EndpointAccessDenied,
                RuntimeDirectConnectDisposition::Fatal,
            },
            ReasonCase{
                RuntimeControlConnectFailureReason::EndpointTransportFailure,
                RuntimeDirectConnectDisposition::RetryConnectOnly,
            },
            ReasonCase{
                RuntimeControlConnectFailureReason::HandshakeDeadlineExpired,
                RuntimeDirectConnectDisposition::RetryConnectOnly,
            },
            ReasonCase{ RuntimeControlConnectFailureReason::HandshakeRejected, RuntimeDirectConnectDisposition::Fatal },
            ReasonCase{
                RuntimeControlConnectFailureReason::HandshakeMalformed,
                RuntimeDirectConnectDisposition::Fatal,
            },
            ReasonCase{
                RuntimeControlConnectFailureReason::HandshakeTransportFailure,
                RuntimeDirectConnectDisposition::RetryConnectOnly,
            },
            ReasonCase{
                RuntimeControlConnectFailureReason::LaunchLockTimeout,
                RuntimeDirectConnectDisposition::RetryConnectOnly,
            },
            ReasonCase{ RuntimeControlConnectFailureReason::LaunchFailed, RuntimeDirectConnectDisposition::Fatal },
            ReasonCase{
                RuntimeControlConnectFailureReason::LaunchedProcessExited,
                RuntimeDirectConnectDisposition::RetryConnectOnly,
            },
            ReasonCase{
                RuntimeControlConnectFailureReason::ActivationWaitFailed,
                RuntimeDirectConnectDisposition::Fatal,
            },
            ReasonCase{ RuntimeControlConnectFailureReason::ResourceFailure, RuntimeDirectConnectDisposition::Fatal },
            ReasonCase{ RuntimeControlConnectFailureReason::Unclassified, RuntimeDirectConnectDisposition::Fatal },
        };

        for (const auto& test : cases)
        {
            const auto status = test.Reason == RuntimeControlConnectFailureReason::Cancelled ?
                                    RuntimeTransportStatus::Cancelled :
                                    RuntimeTransportStatus::SystemError;
            const auto result = FailureResult(status, test.Reason);
            VERIFY_IS_TRUE(
                EvaluateRuntimeDirectConnectDisposition(result) == test.Expected);
        }
    }
}
