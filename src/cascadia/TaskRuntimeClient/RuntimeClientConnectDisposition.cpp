// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientConnectDisposition.h"

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeClientConnectDisposition EvaluateRuntimeClientConnectDisposition(
        const RuntimeControlConnectionResult& result) noexcept
    {
        switch (result.FailureReason)
        {
        case RuntimeControlConnectFailureReason::LaunchFailed:
        case RuntimeControlConnectFailureReason::ActivationWaitFailed:
            return RuntimeClientConnectDisposition::Fatal;
        case RuntimeControlConnectFailureReason::LaunchLockTimeout:
        case RuntimeControlConnectFailureReason::LaunchedProcessExited:
            return RuntimeClientConnectDisposition::Retry;
        default:
            break;
        }

        switch (result.Status)
        {
        case RuntimeTransportStatus::InvalidConfiguration:
        case RuntimeTransportStatus::ProtocolRejected:
        case RuntimeTransportStatus::MalformedFrame:
            return RuntimeClientConnectDisposition::Fatal;
        case RuntimeTransportStatus::SystemError:
            switch (result.SystemError)
            {
            case ERROR_ACCESS_DENIED:
            case ERROR_BAD_PATHNAME:
            case ERROR_INVALID_HANDLE:
            case ERROR_INVALID_NAME:
            case ERROR_INVALID_PARAMETER:
                return RuntimeClientConnectDisposition::Fatal;
            default:
                return RuntimeClientConnectDisposition::Retry;
            }
        default:
            return RuntimeClientConnectDisposition::Retry;
        }
    }
}
