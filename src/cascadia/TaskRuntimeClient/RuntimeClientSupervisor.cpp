// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"
#include "RuntimeClientThreads.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime::details
{
    namespace
    {
        bool _fatalConnectFailure(
            const RuntimeTransportStatus status,
            const DWORD systemError) noexcept
        {
            switch (status)
            {
            case RuntimeTransportStatus::InvalidConfiguration:
            case RuntimeTransportStatus::ProtocolRejected:
            case RuntimeTransportStatus::MalformedFrame:
                return true;
            case RuntimeTransportStatus::Succeeded:
            case RuntimeTransportStatus::Cancelled:
            case RuntimeTransportStatus::Timeout:
            case RuntimeTransportStatus::Disconnected:
                return false;
            case RuntimeTransportStatus::SystemError:
                switch (systemError)
                {
                case ERROR_ACCESS_DENIED:
                case ERROR_BAD_PATHNAME:
                case ERROR_INVALID_HANDLE:
                case ERROR_INVALID_NAME:
                case ERROR_INVALID_PARAMETER:
                    return true;
                default:
                    return false;
                }
            default:
                return false;
            }
        }

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
            const auto retired = Epochs.BeginRetire(epoch->Epoch, cause);
            if (retired.Status == RuntimeClientEpochRetireStatus::Started)
            {
                epoch->RetireCause = retired.Cause;
                epoch->FailureStatus = transportStatus;
                epoch->SystemError = systemError;
                try
                {
                    epoch->Diagnostic.assign(diagnostic);
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
                    if (_fatalConnectFailure(connected.Status, connected.SystemError))
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
                    const auto delay = Dependencies.Backoff->DelayForFailure(failureCount);
                    if (WaitForSingleObject(StopEvent.Get(), delay) == WAIT_OBJECT_0)
                    {
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
                    const auto delay = Dependencies.Backoff->DelayForFailure(failureCount);
                    if (WaitForSingleObject(StopEvent.Get(), delay) == WAIT_OBJECT_0)
                    {
                        break;
                    }
                    continue;
                }

                bool published{};
                bool exhausted{};
                {
                    std::scoped_lock lock{ Mutex };
                    if (ClientState != TaskRuntimeClientState::Stopping &&
                        WaitForSingleObject(StopEvent.Get(), 0) == WAIT_TIMEOUT)
                    {
                        const auto activated = Epochs.Activate();
                        if (activated.Status == RuntimeClientEpochActivationStatus::Activated)
                        {
                            epoch->Epoch = activated.Epoch;
                            Active = epoch;
                            CurrentRuntimeInstanceId = epoch->Hello.RuntimeInstanceId;
                            ClientState = TaskRuntimeClientState::Ready;
                            published = true;
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
                    if (exhausted)
                    {
                        {
                            std::scoped_lock lock{ Mutex };
                            FailAllLocked(
                                RuntimeClientRequestOutcome::ProtocolError,
                                RuntimeTransportStatus::SystemError,
                                ERROR_ARITHMETIC_OVERFLOW,
                                "Client epoch identifier space is exhausted",
                                completions);
                            FailSubscriptionsLocked(
                                RuntimeClientSubscriptionEventKind::Faulted,
                                RuntimeTransportStatus::SystemError,
                                ERROR_ARITHMETIC_OVERFLOW,
                                "Client epoch identifier space is exhausted",
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

                const std::array waits{ StopEvent.Get(), epoch->RetireRequested.Get() };
                const auto wait = WaitForMultipleObjects(
                    static_cast<DWORD>(waits.size()),
                    waits.data(),
                    FALSE,
                    INFINITE);
                if (wait == WAIT_OBJECT_0)
                {
                    RequestEpochRetire(
                        epoch,
                        RuntimeClientRetireCause::StopRequested,
                        RuntimeTransportStatus::Cancelled,
                        ERROR_OPERATION_ABORTED,
                        "Task Runtime client is stopping");
                }
                else if (wait != WAIT_OBJECT_0 + 1)
                {
                    RequestEpochRetire(
                        epoch,
                        RuntimeClientRetireCause::ProtocolError,
                        RuntimeTransportStatus::SystemError,
                        GetLastError(),
                        "Client supervisor wait failed");
                }

                workers.JoinAndClose();

                auto& retries = RetryBuffer;
                retries.clear();
                auto& retired = RetiredBuffer;
                retired.clear();
                bool stopping{};
                bool protocolFault{};
                uint32_t delay{};
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

                if (failureCount != 0)
                {
                    delay = Dependencies.Backoff->DelayForFailure(failureCount);
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
                if (WaitForSingleObject(StopEvent.Get(), delay) == WAIT_OBJECT_0)
                {
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
