// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

#include "RuntimeControlTransport.h"

#include <algorithm>

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

    RuntimeClientHeartbeatMonitorResult RuntimeClientSharedState::MonitorHeartbeat(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch) noexcept
    {
        if (!epoch)
        {
            return RuntimeClientHeartbeatMonitorResult::Failed;
        }

        const auto retire = [&](const RuntimeClientRetireCause cause,
                                const RuntimeTransportStatus transportStatus,
                                const DWORD systemError,
                                const std::string_view diagnostic,
                                const RuntimeClientHeartbeatMonitorResult result) noexcept {
            RequestEpochRetire(epoch, cause, transportStatus, systemError, diagnostic);
            return result;
        };
        const std::array wakeEvents{
            StopEvent.Get(),
            epoch->RetireRequested.Get(),
            epoch->InboundActivity.Get(),
        };

        for (;;)
        {
            if (_stopRequested(*this))
            {
                return retire(
                    RuntimeClientRetireCause::StopRequested,
                    RuntimeTransportStatus::Cancelled,
                    ERROR_OPERATION_ABORTED,
                    "Task Runtime client is stopping",
                    RuntimeClientHeartbeatMonitorResult::StopRequested);
            }
            if (WaitForSingleObject(epoch->RetireRequested.Get(), 0) == WAIT_OBJECT_0)
            {
                return RuntimeClientHeartbeatMonitorResult::EpochRetired;
            }
            // Consume an already-signalled auto-reset activity wake before
            // taking the next decision. The heartbeat window was updated first.
            (void)WaitForSingleObject(epoch->InboundActivity.Get(), 0);

            RuntimeClientHeartbeatDecision decision;
            try
            {
                const auto now = Dependencies.Scheduler->Now();
                std::scoped_lock lock{ Mutex };
                if (!Active || Active.get() != epoch.get() ||
                    !Epochs.IsActive(epoch->Epoch))
                {
                    return RuntimeClientHeartbeatMonitorResult::EpochRetired;
                }
                const auto view = Heartbeat.ActiveView();
                const auto decisionNow = view ? (std::max)(now, view->LastInbound) : now;
                decision = Heartbeat.Decide(decisionNow);
            }
            catch (...)
            {
                decision.Action = RuntimeClientHeartbeatAction::Failed;
            }

            if (decision.Action == RuntimeClientHeartbeatAction::QueuePing)
            {
                WriterWake.notify_all();
                continue;
            }
            if (decision.Action == RuntimeClientHeartbeatAction::Timeout)
            {
                if (_stopRequested(*this))
                {
                    return retire(
                        RuntimeClientRetireCause::StopRequested,
                        RuntimeTransportStatus::Cancelled,
                        ERROR_OPERATION_ABORTED,
                        "Task Runtime client is stopping",
                        RuntimeClientHeartbeatMonitorResult::StopRequested);
                }
                if (WaitForSingleObject(epoch->RetireRequested.Get(), 0) == WAIT_OBJECT_0)
                {
                    return RuntimeClientHeartbeatMonitorResult::EpochRetired;
                }
                return retire(
                    RuntimeClientRetireCause::HeartbeatTimeout,
                    RuntimeTransportStatus::Timeout,
                    ERROR_TIMEOUT,
                    "Task Runtime heartbeat silence deadline expired",
                    RuntimeClientHeartbeatMonitorResult::EpochRetired);
            }
            if (decision.Action == RuntimeClientHeartbeatAction::Failed)
            {
                if (_stopRequested(*this))
                {
                    return retire(
                        RuntimeClientRetireCause::StopRequested,
                        RuntimeTransportStatus::Cancelled,
                        ERROR_OPERATION_ABORTED,
                        "Task Runtime client is stopping",
                        RuntimeClientHeartbeatMonitorResult::StopRequested);
                }
                if (WaitForSingleObject(epoch->RetireRequested.Get(), 0) == WAIT_OBJECT_0)
                {
                    return RuntimeClientHeartbeatMonitorResult::EpochRetired;
                }
                return retire(
                    RuntimeClientRetireCause::ProtocolError,
                    RuntimeTransportStatus::SystemError,
                    ERROR_INVALID_STATE,
                    "Task Runtime heartbeat state is invalid",
                    RuntimeClientHeartbeatMonitorResult::Failed);
            }

            RuntimeClientScheduledWaitResult waited;
            try
            {
                waited = Dependencies.Scheduler->WaitUntil(decision.Deadline, wakeEvents);
            }
            catch (...)
            {
                waited.Status = RuntimeClientScheduledWaitStatus::Failed;
                waited.SystemError = ERROR_UNHANDLED_EXCEPTION;
            }
            if (waited.Status == RuntimeClientScheduledWaitStatus::DeadlineReached)
            {
                continue;
            }
            if (waited.Status == RuntimeClientScheduledWaitStatus::WakeSignaled)
            {
                if (waited.WakeIndex == 0)
                {
                    return retire(
                        RuntimeClientRetireCause::StopRequested,
                        RuntimeTransportStatus::Cancelled,
                        ERROR_OPERATION_ABORTED,
                        "Task Runtime client is stopping",
                        RuntimeClientHeartbeatMonitorResult::StopRequested);
                }
                if (waited.WakeIndex == 1)
                {
                    return RuntimeClientHeartbeatMonitorResult::EpochRetired;
                }
                if (waited.WakeIndex == 2)
                {
                    continue;
                }
            }
            if (_stopRequested(*this))
            {
                return retire(
                    RuntimeClientRetireCause::StopRequested,
                    RuntimeTransportStatus::Cancelled,
                    ERROR_OPERATION_ABORTED,
                    "Task Runtime client is stopping",
                    RuntimeClientHeartbeatMonitorResult::StopRequested);
            }
            if (WaitForSingleObject(epoch->RetireRequested.Get(), 0) == WAIT_OBJECT_0)
            {
                return RuntimeClientHeartbeatMonitorResult::EpochRetired;
            }
            return retire(
                RuntimeClientRetireCause::ProtocolError,
                RuntimeTransportStatus::SystemError,
                waited.SystemError == ERROR_SUCCESS ? ERROR_GEN_FAILURE : waited.SystemError,
                "Task Runtime heartbeat scheduler wait failed",
                RuntimeClientHeartbeatMonitorResult::Failed);
        }
    }
}
