// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeLaunchingConnectorResults.h"

#include "RuntimeActivationDeadline.h"
#include "RuntimeProcessExitCode.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    RuntimeControlConnectionResult ActivationFailure(
        const RuntimeTransportStatus status,
        const RuntimeControlConnectFailureReason reason,
        const DWORD systemError,
        std::string error,
        const RuntimeControlConnectPhase phase)
    {
        RuntimeControlConnectionResult result;
        result.Status = status;
        result.FailureReason = reason;
        result.SystemError = systemError;
        result.Error = std::move(error);
        result.Phase = phase;
        return result;
    }

    uint32_t ActivationRemaining(
        IRuntimeActivationServices& services,
        const RuntimeActivationDeadline deadline) noexcept
    {
        const auto remaining = RuntimeActivationRemainingMilliseconds(
            deadline,
            services.Now());
        return static_cast<uint32_t>((std::min)(
            remaining,
            static_cast<RuntimeActivationMonotonicMilliseconds>(
                MaximumRuntimeTransportTimeoutMilliseconds)));
    }

    RuntimeControlConnectionResult ActivationDeadlineFailure()
    {
        return ActivationFailure(
            RuntimeTransportStatus::Timeout,
            RuntimeControlConnectFailureReason::DeadlineExpired,
            WAIT_TIMEOUT,
            "Runtime activation deadline expired");
    }

    RuntimeControlConnectionResult ActivationLockFailure(
        const RuntimeLaunchLockLeaseResult& lock)
    {
        switch (lock.Status)
        {
        case RuntimeLaunchLockStatus::Cancelled:
            return ActivationFailure(
                RuntimeTransportStatus::Cancelled,
                RuntimeControlConnectFailureReason::Cancelled,
                lock.SystemError,
                lock.Error);
        case RuntimeLaunchLockStatus::TimedOut:
            return ActivationFailure(
                RuntimeTransportStatus::Timeout,
                RuntimeControlConnectFailureReason::LaunchLockTimeout,
                lock.SystemError,
                lock.Error);
        case RuntimeLaunchLockStatus::InvalidIdentity:
        case RuntimeLaunchLockStatus::InvalidDeadline:
        case RuntimeLaunchLockStatus::SecurityFailure:
            return ActivationFailure(
                RuntimeTransportStatus::InvalidConfiguration,
                RuntimeControlConnectFailureReason::LaunchFailed,
                lock.SystemError,
                lock.Error);
        case RuntimeLaunchLockStatus::SystemError:
        case RuntimeLaunchLockStatus::Acquired:
        default:
            return ActivationFailure(
                RuntimeTransportStatus::SystemError,
                RuntimeControlConnectFailureReason::LaunchFailed,
                lock.SystemError,
                lock.Error.empty() ? "Runtime launch-lock acquisition failed" : lock.Error);
        }
    }

    namespace
    {
        bool _fatalLaunchStatus(const RuntimeProcessLaunchStatus status) noexcept
        {
            switch (status)
            {
            case RuntimeProcessLaunchStatus::InvalidRequest:
            case RuntimeProcessLaunchStatus::InvalidExecutablePath:
            case RuntimeProcessLaunchStatus::CommandLineTooLong:
            case RuntimeProcessLaunchStatus::ParentJobBreakawayDenied:
            case RuntimeProcessLaunchStatus::ChildStillInJob:
                return true;
            default:
                return false;
            }
        }

    }

    RuntimeControlConnectionResult ActivationLaunchFailure(
        const RuntimeProcessLaunchResult& launched)
    {
        if (launched.Status == RuntimeProcessLaunchStatus::CancelledBeforeCreate)
        {
            return ActivationFailure(
                RuntimeTransportStatus::Cancelled,
                RuntimeControlConnectFailureReason::Cancelled,
                launched.SystemError,
                launched.Error);
        }
        return ActivationFailure(
            _fatalLaunchStatus(launched.Status) ?
                RuntimeTransportStatus::InvalidConfiguration :
                RuntimeTransportStatus::SystemError,
            RuntimeControlConnectFailureReason::LaunchFailed,
            launched.SystemError,
            launched.Error.empty() ? "Task Runtime process launch failed" : launched.Error);
    }

    std::optional<RuntimeControlConnectionResult> ObserveRuntimeProcessExit(
        RuntimeProcessObservation& process)
    {
        const auto exit = process.QueryExit();
        if (exit.Status == RuntimeProcessExitStatus::Running)
        {
            return std::nullopt;
        }
        if (exit.Status == RuntimeProcessExitStatus::Exited)
        {
            const auto deterministic =
                IsDeterministicRuntimeStartupFailureExitCode(exit.ExitCode);
            auto result = ActivationFailure(
                deterministic ?
                    RuntimeTransportStatus::InvalidConfiguration :
                    RuntimeTransportStatus::Disconnected,
                deterministic ?
                    RuntimeControlConnectFailureReason::LaunchFailed :
                    RuntimeControlConnectFailureReason::LaunchedProcessExited,
                ERROR_PROCESS_ABORTED,
                "Task Runtime exited before Hello with code " + std::to_string(exit.ExitCode));
            result.LaunchedProcessExitCode = exit.ExitCode;
            return result;
        }
        return ActivationFailure(
            RuntimeTransportStatus::SystemError,
            RuntimeControlConnectFailureReason::ActivationWaitFailed,
            exit.SystemError,
            "Task Runtime process observation failed");
    }
}
