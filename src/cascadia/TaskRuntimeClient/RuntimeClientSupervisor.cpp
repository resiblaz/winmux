// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"
#include "RuntimeClientConnectDisposition.h"
#include "RuntimeClientThreads.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime::details
{
    namespace
    {
        uint32_t _incrementFailure(const uint32_t value) noexcept
        {
            return value == std::numeric_limits<uint32_t>::max() ? value : value + 1;
        }

    }

    void RuntimeClientSharedState::RequestEpochRetire(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        const RuntimeClientRetireCause cause,
        const RuntimeTransportStatus transportStatus,
        const DWORD systemError,
        const std::string_view diagnostic) noexcept
    {
        if (!epoch)
        {
            return;
        }
        bool cancel{};
        try
        {
            std::scoped_lock lock{ Mutex };
            if (!Active || Active.get() != epoch.get())
            {
                return;
            }
            const auto stopping = ClientState == TaskRuntimeClientState::Stopping ||
                                  WaitForSingleObject(StopEvent.Get(), 0) == WAIT_OBJECT_0;
            const auto effectiveCause = stopping ? RuntimeClientRetireCause::StopRequested : cause;
            const auto effectiveStatus = stopping ? RuntimeTransportStatus::Cancelled : transportStatus;
            const auto effectiveSystemError = stopping ? ERROR_OPERATION_ABORTED : systemError;
            const auto effectiveDiagnostic = stopping ?
                                                 std::string_view{ "Task Runtime client is stopping" } :
                                                 diagnostic;
            const auto retired = Epochs.BeginRetire(epoch->Epoch, effectiveCause);
            if (retired.Status == RuntimeClientEpochRetireStatus::Started)
            {
                epoch->RetireCause = retired.Cause;
                epoch->FailureStatus = effectiveStatus;
                epoch->SystemError = effectiveSystemError;
                try
                {
                    epoch->Diagnostic.assign(effectiveDiagnostic);
                }
                catch (...)
                {
                    epoch->Diagnostic.clear();
                }
                cancel = true;
            }
            else if (retired.Status == RuntimeClientEpochRetireStatus::AlreadyRetiring)
            {
                cancel = true;
            }
        }
        catch (...)
        {
            cancel = true;
        }
        if (cancel)
        {
            epoch->Cancel();
            WriterWake.notify_all();
        }
    }

    void RuntimeClientSharedState::RunSupervisor() noexcept
    {
        {
            std::scoped_lock lock{ Mutex };
            SupervisorThreadId = std::this_thread::get_id();
        }

        auto& completions = CompletionBuffer;
        completions.clear();
        try
        {
            while (WaitForSingleObject(StopEvent.Get(), 0) == WAIT_TIMEOUT)
            {
                {
                    std::scoped_lock lock{ Mutex };
                    if (ClientState == TaskRuntimeClientState::Stopping)
                    {
                        break;
                    }
                    ClientState = TaskRuntimeClientState::Connecting;
                }

                auto connected = Dependencies.Connector->Connect(
                    Configuration.Identity,
                    Configuration.Hello,
                    Configuration.ConnectTimeoutMilliseconds,
                    StopEvent.Get());
                if (WaitForSingleObject(StopEvent.Get(), 0) == WAIT_OBJECT_0)
                {
                    if (connected.Connection)
                    {
                        connected.Connection->CancelPendingIo();
                        connected.Connection->Close();
                    }
                    break;
                }

                if (!connected.Succeeded())
                {
                    if (EvaluateRuntimeClientConnectDisposition(connected) ==
                        RuntimeClientConnectDisposition::Fatal)
                    {
                        {
                            std::scoped_lock lock{ Mutex };
                            AdmissionOpen = false;
                            ClientState = TaskRuntimeClientState::Faulted;
                            FailAllLocked(
                                RuntimeClientRequestOutcome::ProtocolError,
                                connected.Status,
                                connected.SystemError,
                                connected.Error,
                                completions);
                            FailSubscriptionsLocked(
                                RuntimeClientSubscriptionEventKind::Faulted,
                                connected.Status,
                                connected.SystemError,
                                connected.Error,
                                SubscriptionDeliveryBuffer);
                        }
                        Deliver(completions);
                        DeliverSubscriptions(SubscriptionDeliveryBuffer);
                        WaitForSingleObject(StopEvent.Get(), INFINITE);
                        break;
                    }

                    uint32_t failureCount{};
                    {
                        std::scoped_lock lock{ Mutex };
                        ConsecutiveFailures = _incrementFailure(ConsecutiveFailures);
                        ClientState = TaskRuntimeClientState::Backoff;
                        failureCount = ConsecutiveFailures;
                    }
                    const auto backoff = WaitForReconnectBackoff(failureCount);
                    if (backoff != RuntimeClientBackoffWaitResult::Elapsed)
                    {
                        if (backoff == RuntimeClientBackoffWaitResult::Failed)
                        {
                            WaitForSingleObject(StopEvent.Get(), INFINITE);
                        }
                        break;
                    }
                    continue;
                }

                std::shared_ptr<RuntimeClientEpochContext> epoch;
                try
                {
                    epoch = std::make_shared<RuntimeClientEpochContext>(
                        std::move(connected.Response),
                        std::move(connected.Connection));
                }
                catch (...)
                {
                }
                if (!epoch || !epoch->Valid())
                {
                    if (epoch)
                    {
                        epoch->Cancel();
                        epoch->Close();
                    }
                    uint32_t failureCount{};
                    {
                        std::scoped_lock lock{ Mutex };
                        ConsecutiveFailures = _incrementFailure(ConsecutiveFailures);
                        ClientState = TaskRuntimeClientState::Backoff;
                        failureCount = ConsecutiveFailures;
                    }
                    const auto backoff = WaitForReconnectBackoff(failureCount);
                    if (backoff != RuntimeClientBackoffWaitResult::Elapsed)
                    {
                        if (backoff == RuntimeClientBackoffWaitResult::Failed)
                        {
                            WaitForSingleObject(StopEvent.Get(), INFINITE);
                        }
                        break;
                    }
                    continue;
                }

                RuntimeClientMonotonicMilliseconds heartbeatNow{};
                bool heartbeatClockReady{};
                try
                {
                    heartbeatNow = Dependencies.Scheduler->Now();
                    heartbeatClockReady = true;
                }
                catch (...)
                {
                }

                bool published{};
                bool exhausted{};
                bool heartbeatFailed{};
                {
                    std::scoped_lock lock{ Mutex };
                    if (ClientState != TaskRuntimeClientState::Stopping &&
                        WaitForSingleObject(StopEvent.Get(), 0) == WAIT_TIMEOUT)
                    {
                        const auto activated = Epochs.Activate();
                        if (activated.Status == RuntimeClientEpochActivationStatus::Activated)
                        {
                            epoch->Epoch = activated.Epoch;
                            if (heartbeatClockReady &&
                                Heartbeat.Initialize(
                                    epoch->Epoch,
                                    heartbeatNow,
                                    epoch->Hello.HeartbeatMilliseconds))
                            {
                                Active = epoch;
                                CurrentRuntimeInstanceId = epoch->Hello.RuntimeInstanceId;
                                ClientState = TaskRuntimeClientState::Ready;
                                published = true;
                            }
                            else
                            {
                                (void)Epochs.BeginRetire(
                                    epoch->Epoch,
                                    RuntimeClientRetireCause::ProtocolError);
                                (void)Epochs.CompleteRetire(epoch->Epoch);
                                Heartbeat.Retire();
                                heartbeatFailed = true;
                                AdmissionOpen = false;
                                ClientState = TaskRuntimeClientState::Faulted;
                            }
                        }
                        else if (activated.Status == RuntimeClientEpochActivationStatus::Exhausted)
                        {
                            exhausted = true;
                            AdmissionOpen = false;
                            ClientState = TaskRuntimeClientState::Faulted;
                        }
                    }
                }
                if (!published)
                {
                    epoch->Cancel();
                    epoch->Close();
                    if (exhausted || heartbeatFailed)
                    {
                        const auto systemError = exhausted ?
                                                     ERROR_ARITHMETIC_OVERFLOW :
                                                     ERROR_INVALID_STATE;
                        const auto diagnostic = exhausted ?
                                                    "Client epoch identifier space is exhausted" :
                                                    "Client heartbeat could not initialize";
                        {
                            std::scoped_lock lock{ Mutex };
                            FailAllLocked(
                                RuntimeClientRequestOutcome::ProtocolError,
                                RuntimeTransportStatus::SystemError,
                                systemError,
                                diagnostic,
                                completions);
                            FailSubscriptionsLocked(
                                RuntimeClientSubscriptionEventKind::Faulted,
                                RuntimeTransportStatus::SystemError,
                                systemError,
                                diagnostic,
                                SubscriptionDeliveryBuffer);
                        }
                        Deliver(completions);
                        DeliverSubscriptions(SubscriptionDeliveryBuffer);
                        WaitForSingleObject(StopEvent.Get(), INFINITE);
                    }
                    break;
                }

                RuntimeClientEpochWorkers workers{ epoch };
                try
                {
                    const auto state = shared_from_this();
                    workers.Reader = StartRuntimeClientEpochWorker(
                        epoch,
                        [state, epoch] { state->RunReader(epoch); });
                    workers.Writer = StartRuntimeClientEpochWorker(
                        epoch,
                        [state, epoch] { state->RunWriter(epoch); });
                }
                catch (...)
                {
                    RequestEpochRetire(
                        epoch,
                        RuntimeClientRetireCause::TransportFailure,
                        RuntimeTransportStatus::SystemError,
                        ERROR_NOT_ENOUGH_MEMORY,
                        "Client IO worker could not start");
                }

                (void)MonitorHeartbeat(epoch);

                workers.JoinAndClose();

                auto& retries = RetryBuffer;
                retries.clear();
                auto& retired = RetiredBuffer;
                retired.clear();
                bool stopping{};
                bool protocolFault{};
                uint32_t failureCount{};
                {
                    std::scoped_lock lock{ Mutex };
                    stopping = ClientState == TaskRuntimeClientState::Stopping ||
                               WaitForSingleObject(StopEvent.Get(), 0) == WAIT_OBJECT_0 ||
                               epoch->RetireCause == RuntimeClientRetireCause::StopRequested;
                    protocolFault = epoch->RetireCause == RuntimeClientRetireCause::ProtocolError;
                    if (Requests.RetireEpochInto(epoch->Epoch, retired))
                    {
                        ProcessRetiredLocked(
                            retired,
                            stopping,
                            protocolFault,
                            epoch->FailureStatus,
                            epoch->SystemError,
                            epoch->Diagnostic,
                            retries,
                            completions);
                    }
                    else
                    {
                        protocolFault = true;
                        FailAllLocked(
                            RuntimeClientRequestOutcome::OutcomeUnknown,
                            RuntimeTransportStatus::SystemError,
                            ERROR_NOT_ENOUGH_MEMORY,
                            "Client retirement buffer is unavailable",
                            completions);
                    }
                    RetireSubscriptionsLocked(epoch->Epoch, stopping, SubscriptionDeliveryBuffer);
                    Heartbeat.Retire();
                    retired.clear();
                    Requests.ClearRetiredEpochTombstones(epoch->Epoch);
                    (void)Epochs.CompleteRetire(epoch->Epoch);
                    if (Active && Active.get() == epoch.get())
                    {
                        Active.reset();
                        CurrentRuntimeInstanceId.clear();
                    }

                    if (protocolFault && !stopping)
                    {
                        AdmissionOpen = false;
                        ClientState = TaskRuntimeClientState::Faulted;
                        FailAllLocked(
                            RuntimeClientRequestOutcome::ProtocolError,
                            epoch->FailureStatus,
                            epoch->SystemError,
                            epoch->Diagnostic,
                            completions);
                        FailSubscriptionsLocked(
                            RuntimeClientSubscriptionEventKind::Faulted,
                            epoch->FailureStatus,
                            epoch->SystemError,
                            epoch->Diagnostic,
                            SubscriptionDeliveryBuffer);
                        retries.clear();
                    }
                    else if (stopping)
                    {
                        ClientState = TaskRuntimeClientState::Stopping;
                        retries.clear();
                    }
                    else
                    {
                        ConsecutiveFailures = _incrementFailure(ConsecutiveFailures);
                        ClientState = TaskRuntimeClientState::Backoff;
                        failureCount = ConsecutiveFailures;
                    }
                }

                if (!retries.empty())
                {
                    QueueRetries(retries, completions);
                }
                Deliver(completions);
                DeliverSubscriptions(SubscriptionDeliveryBuffer);
                if (protocolFault && !stopping)
                {
                    WaitForSingleObject(StopEvent.Get(), INFINITE);
                    break;
                }
                if (stopping)
                {
                    break;
                }
                const auto backoff = WaitForReconnectBackoff(failureCount);
                if (backoff != RuntimeClientBackoffWaitResult::Elapsed)
                {
                    if (backoff == RuntimeClientBackoffWaitResult::Failed)
                    {
                        WaitForSingleObject(StopEvent.Get(), INFINITE);
                    }
                    break;
                }
            }
        }
        catch (...)
        {
            std::shared_ptr<RuntimeClientEpochContext> active;
            try
            {
                {
                    std::scoped_lock lock{ Mutex };
                    active = Active;
                }
                if (active)
                {
                    RequestEpochRetire(
                        active,
                        RuntimeClientRetireCause::ProtocolError,
                        RuntimeTransportStatus::SystemError,
                        ERROR_UNHANDLED_EXCEPTION,
                        "Unexpected client supervisor failure");
                    active->Cancel();
                    active->Close();
                }

                std::scoped_lock lock{ Mutex };
                auto& retired = RetiredBuffer;
                retired.clear();
                auto& retries = RetryBuffer;
                retries.clear();
                if (active && Active && Active.get() == active.get())
                {
                    if (Requests.RetireEpochInto(active->Epoch, retired))
                    {
                        ProcessRetiredLocked(
                            retired,
                            false,
                            true,
                            RuntimeTransportStatus::SystemError,
                            ERROR_UNHANDLED_EXCEPTION,
                            "Unexpected client supervisor failure",
                            retries,
                            completions);
                    }
                    else
                    {
                        FailAllLocked(
                            RuntimeClientRequestOutcome::OutcomeUnknown,
                            RuntimeTransportStatus::SystemError,
                            ERROR_UNHANDLED_EXCEPTION,
                            "Unexpected client supervisor failure",
                            completions);
                    }
                    retired.clear();
                    retries.clear();
                    (void)Epochs.BeginRetire(active->Epoch, RuntimeClientRetireCause::ProtocolError);
                    (void)Epochs.CompleteRetire(active->Epoch);
                    Heartbeat.Retire();
                    Active.reset();
                    CurrentRuntimeInstanceId.clear();
                }
                AdmissionOpen = false;
                ClientState = TaskRuntimeClientState::Faulted;
                FailAllLocked(
                    RuntimeClientRequestOutcome::ProtocolError,
                    RuntimeTransportStatus::SystemError,
                    ERROR_UNHANDLED_EXCEPTION,
                    "Unexpected client supervisor failure",
                    completions);
                FailSubscriptionsLocked(
                    RuntimeClientSubscriptionEventKind::Faulted,
                    RuntimeTransportStatus::SystemError,
                    ERROR_UNHANDLED_EXCEPTION,
                    "Unexpected client supervisor failure",
                    SubscriptionDeliveryBuffer);
            }
            catch (...)
            {
            }
            Deliver(completions);
            DeliverSubscriptions(SubscriptionDeliveryBuffer);
            WaitForSingleObject(StopEvent.Get(), INFINITE);
        }

        FinishStopped(completions);
        Deliver(completions);
        DeliverSubscriptions(SubscriptionDeliveryBuffer);
    }
}
