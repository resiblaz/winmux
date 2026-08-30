// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeDirectConnectDisposition.h"

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeDirectConnectDisposition EvaluateRuntimeDirectConnectDisposition(
        const RuntimeControlConnectionResult& result) noexcept
    {
        if (result.Succeeded())
        {
            return RuntimeDirectConnectDisposition::Connected;
        }
        if (result.FailureReason == RuntimeControlConnectFailureReason::EndpointAbsent &&
            result.Phase == RuntimeControlConnectPhase::Connecting &&
            result.Status == RuntimeTransportStatus::Disconnected &&
            result.SystemError == ERROR_FILE_NOT_FOUND && !result.Connection)
        {
            return RuntimeDirectConnectDisposition::MayLaunch;
        }
        switch (result.FailureReason)
        {
        case RuntimeControlConnectFailureReason::Cancelled:
            return RuntimeDirectConnectDisposition::Cancelled;
        case RuntimeControlConnectFailureReason::EndpointBusy:
        case RuntimeControlConnectFailureReason::DeadlineExpired:
        case RuntimeControlConnectFailureReason::EndpointTransportFailure:
        case RuntimeControlConnectFailureReason::HandshakeDeadlineExpired:
        case RuntimeControlConnectFailureReason::HandshakeTransportFailure:
        case RuntimeControlConnectFailureReason::LaunchLockTimeout:
        case RuntimeControlConnectFailureReason::LaunchedProcessExited:
            return RuntimeDirectConnectDisposition::RetryConnectOnly;
        case RuntimeControlConnectFailureReason::None:
        case RuntimeControlConnectFailureReason::EndpointAbsent:
        case RuntimeControlConnectFailureReason::InvalidConfiguration:
        case RuntimeControlConnectFailureReason::EndpointAccessDenied:
        case RuntimeControlConnectFailureReason::HandshakeRejected:
        case RuntimeControlConnectFailureReason::HandshakeMalformed:
        case RuntimeControlConnectFailureReason::LaunchFailed:
        case RuntimeControlConnectFailureReason::ActivationWaitFailed:
        case RuntimeControlConnectFailureReason::ResourceFailure:
        case RuntimeControlConnectFailureReason::Unclassified:
        default:
            return RuntimeDirectConnectDisposition::Fatal;
        }
    }
}
