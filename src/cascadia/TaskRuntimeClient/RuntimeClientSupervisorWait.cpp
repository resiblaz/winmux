// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

#include "RuntimeControlTransport.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    namespace
    {
        bool _stopRequested(const RuntimeClientSharedState& state) noexcept
        {
            return WaitForSingleObject(state.StopEvent.Get(), 0) == WAIT_OBJECT_0;
        }

        bool _faultScheduler(
            RuntimeClientSharedState& state,
            const DWORD systemError,
            const std::string_view diagnostic) noexcept
        {
            try
            {
                {
                    std::scoped_lock lock{ state.Mutex };
                    if (state.ClientState == TaskRuntimeClientState::Stopping ||
                        _stopRequested(state))
                    {
                        return false;
                    }
                    state.AdmissionOpen = false;
                    state.ClientState = TaskRuntimeClientState::Faulted;
                    state.FailAllLocked(
                        RuntimeClientRequestOutcome::ProtocolError,
                        RuntimeTransportStatus::SystemError,
                        systemError,
                        diagnostic,
                        state.CompletionBuffer);
                    state.FailSubscriptionsLocked(
                        RuntimeClientSubscriptionEventKind::Faulted,
                        RuntimeTransportStatus::SystemError,
                        systemError,
                        diagnostic,
                        state.SubscriptionDeliveryBuffer);
                }
                RuntimeClientSharedState::Deliver(state.CompletionBuffer);
                RuntimeClientSharedState::DeliverSubscriptions(state.SubscriptionDeliveryBuffer);
                return true;
            }
            catch (...)
            {
                try
                {
                    std::scoped_lock lock{ state.Mutex };
                    if (state.ClientState == TaskRuntimeClientState::Stopping ||
                        _stopRequested(state))
                    {
                        return false;
                    }
                    state.AdmissionOpen = false;
                    state.ClientState = TaskRuntimeClientState::Faulted;
                }
                catch (...)
                {
                }
                return true;
            }
        }
    }

    RuntimeClientBackoffWaitResult RuntimeClientSharedState::WaitForReconnectBackoff(
        const uint32_t failureCount) noexcept
    {
        if (_stopRequested(*this))
        {
            return RuntimeClientBackoffWaitResult::StopRequested;
        }

        uint32_t delay{};
        try
        {
            delay = Dependencies.Backoff->DelayForFailure(failureCount);
        }
        catch (...)
        {
            delay = 0;
        }
        if (failureCount == 0 || delay == 0 ||
            delay > MaximumRuntimeTransportTimeoutMilliseconds)
        {
            if (_stopRequested(*this))
            {
                return RuntimeClientBackoffWaitResult::StopRequested;
            }
            return _faultScheduler(
                       *this,
                       ERROR_INVALID_PARAMETER,
                       "Runtime client backoff policy returned an invalid delay") ?
                       RuntimeClientBackoffWaitResult::Failed :
                       RuntimeClientBackoffWaitResult::StopRequested;
        }

        RuntimeClientScheduledWaitResult waited;
        try
        {
            const auto now = Dependencies.Scheduler->Now();
            const auto maximum =
                (std::numeric_limits<RuntimeClientMonotonicMilliseconds>::max)();
            if (delay > maximum - now)
            {
                if (_stopRequested(*this))
                {
                    return RuntimeClientBackoffWaitResult::StopRequested;
                }
                return _faultScheduler(
                           *this,
                           ERROR_ARITHMETIC_OVERFLOW,
                           "Runtime client scheduler deadline overflowed") ?
                           RuntimeClientBackoffWaitResult::Failed :
                           RuntimeClientBackoffWaitResult::StopRequested;
            }
            const auto deadline = RuntimeClientDeadlineAfter(now, delay);
            if (deadline <= now)
            {
                if (_stopRequested(*this))
                {
                    return RuntimeClientBackoffWaitResult::StopRequested;
                }
                return _faultScheduler(
                           *this,
                           ERROR_ARITHMETIC_OVERFLOW,
                           "Runtime client scheduler deadline overflowed") ?
                           RuntimeClientBackoffWaitResult::Failed :
                           RuntimeClientBackoffWaitResult::StopRequested;
            }
            const std::array wakeEvents{ StopEvent.Get() };
            waited = Dependencies.Scheduler->WaitUntil(deadline, wakeEvents);
        }
        catch (...)
        {
            waited.Status = RuntimeClientScheduledWaitStatus::Failed;
            waited.SystemError = ERROR_UNHANDLED_EXCEPTION;
        }

        if (waited.Status == RuntimeClientScheduledWaitStatus::WakeSignaled &&
            waited.WakeIndex == 0)
        {
            if (_stopRequested(*this))
            {
                return RuntimeClientBackoffWaitResult::StopRequested;
            }
        }
        if (waited.Status == RuntimeClientScheduledWaitStatus::DeadlineReached)
        {
            // Close the final timeout/signalling race in favor of Stop.
            return _stopRequested(*this) ?
                       RuntimeClientBackoffWaitResult::StopRequested :
                       RuntimeClientBackoffWaitResult::Elapsed;
        }
        if (_stopRequested(*this))
        {
            return RuntimeClientBackoffWaitResult::StopRequested;
        }

        return _faultScheduler(
                   *this,
                   waited.SystemError == ERROR_SUCCESS ? ERROR_GEN_FAILURE : waited.SystemError,
                   "Runtime client scheduler wait failed") ?
                   RuntimeClientBackoffWaitResult::Failed :
                   RuntimeClientBackoffWaitResult::StopRequested;
    }
}
