// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeControlEndpointDiscoveryPolicy
    {
        WaitUntilDeadline,
        ReturnImmediatelyIfAbsent,
    };

    enum class RuntimeControlEndpointObservation
    {
        Unknown,
        Absent,
        Busy,
        Available,
    };

    enum class RuntimeControlConnectFailureReason
    {
        None,
        InvalidConfiguration,
        Cancelled,
        DeadlineExpired,
        EndpointAbsent,
        EndpointBusy,
        EndpointAccessDenied,
        EndpointTransportFailure,
        HandshakeDeadlineExpired,
        HandshakeRejected,
        HandshakeMalformed,
        HandshakeTransportFailure,
        LaunchLockTimeout,
        LaunchFailed,
        LaunchedProcessExited,
        ActivationWaitFailed,
        ResourceFailure,
        Unclassified,
    };
}
