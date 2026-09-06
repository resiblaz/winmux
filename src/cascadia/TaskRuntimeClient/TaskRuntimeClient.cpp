// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskRuntimeClient.h"

#include "RuntimeClientState.h"
#include "RuntimeClientProductConnector.h"
#include "RuntimeClientThreads.h"

#include "ProtocolJson.h"
#include "ProtocolRpcJson.h"
#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        constexpr std::string_view ValidationRequestId{
            "30000000-0000-4000-8000-000000000201"
        };

        bool _validTimeout(const uint32_t value) noexcept
        {
            return value != 0 && value <= MaximumRuntimeTransportTimeoutMilliseconds;
        }

        bool _validConfiguration(const TaskRuntimeClientConfiguration& configuration)
        {
            const auto hello = EncodeHelloRequest(configuration.Hello);
            const auto maximumSize = (std::numeric_limits<size_t>::max)();
            const auto operationCapacityValid =
                configuration.MaximumSubscriptions < maximumSize &&
                configuration.MaximumOperations <=
                    maximumSize - configuration.MaximumSubscriptions - 1;
            const auto totalOperations = operationCapacityValid ?
                                             configuration.MaximumOperations +
                                                 configuration.MaximumSubscriptions + 1 :
                                             maximumSize;
            const auto subscriptionReservationRequired =
                configuration.MaximumRequestBytes != 0 &&
                        configuration.MaximumConcurrentSubscriptionOperations <=
                            (std::numeric_limits<size_t>::max)() / configuration.MaximumRequestBytes ?
                    configuration.MaximumConcurrentSubscriptionOperations *
                        configuration.MaximumRequestBytes :
                    (std::numeric_limits<size_t>::max)();
            return !configuration.Identity.ControlPipeName.empty() &&
                   !configuration.Identity.ProtocolIdentity.empty() &&
                   configuration.Hello.ExpectedRuntimeIdentity ==
                       configuration.Identity.ProtocolIdentity &&
                   hello.Succeeded &&
                   _validTimeout(configuration.ConnectTimeoutMilliseconds) &&
                   _validTimeout(configuration.SendTimeoutMilliseconds) &&
                   _validTimeout(configuration.ReceiveTimeoutMilliseconds) &&
                   configuration.MaximumOperations != 0 &&
                   configuration.MaximumSubscriptions != 0 &&
                   configuration.MaximumSubscriptions <= AbsoluteMaximumRuntimeClientSubscriptions &&
                   configuration.MaximumConcurrentSubscriptionOperations != 0 &&
                   configuration.MaximumConcurrentSubscriptionOperations <= configuration.MaximumSubscriptions &&
                   totalOperations > configuration.MaximumOperations && totalOperations <= 4096 &&
                   configuration.MaximumQueuedBytes != 0 &&
                   configuration.ReservedSubscriptionRequestBytes != 0 &&
                   configuration.MaximumRequestBytes <= configuration.MaximumQueuedBytes &&
                   configuration.ReservedSubscriptionRequestBytes <
                       configuration.MaximumQueuedBytes - configuration.MaximumRequestBytes &&
                   configuration.ReservedSubscriptionRequestBytes >= subscriptionReservationRequired &&
                   configuration.MaximumRequestBytes >= MinimumControlFrameBytes &&
                   configuration.MaximumRequestBytes <= MaximumControlFrameBytes &&
                   configuration.MaximumQueuedBytes - configuration.MaximumRequestBytes -
                           configuration.ReservedSubscriptionRequestBytes >=
                       configuration.MaximumRequestBytes &&
                   configuration.MaximumAggregateSubscriptionEventBytes >=
                       DefaultMaximumSubscriptionStreamBytes &&
                   configuration.MaximumAggregateSubscriptionEventBytes <=
                       AbsoluteMaximumClientSubscriptionAggregateBytes &&
                   configuration.MaximumRememberedRequestIds != 0 &&
                   configuration.MaximumRememberedRequestIds <= 1024 * 1024 &&
                   configuration.DefaultMaximumAttempts != 0 &&
                   configuration.MaximumAttemptsPerOperation != 0 &&
                   configuration.DefaultMaximumAttempts <=
                       configuration.MaximumAttemptsPerOperation &&
                   configuration.MaximumSubscriptionAttempts != 0 &&
                   configuration.MaximumSubscriptionAttempts <= configuration.MaximumAttemptsPerOperation &&
                   configuration.MaximumDiagnosticBytes != 0 &&
                   configuration.MaximumDiagnosticBytes <= 64 * 1024;
        }

        size_t _requestRegistryCapacity(const TaskRuntimeClientConfiguration& configuration) noexcept
        {
            if (configuration.MaximumSubscriptions >=
                (std::numeric_limits<size_t>::max)() - configuration.MaximumOperations)
            {
                return 0;
            }
            return configuration.MaximumOperations + configuration.MaximumSubscriptions + 1;
        }

        RuntimeClientRequestHandle _readyHandle(RuntimeClientRequestResult result)
        {
            auto promise = std::make_shared<std::promise<RuntimeClientRequestResult>>();
            auto future = promise->get_future().share();
            promise->set_value(std::move(result));
            return { false, 0, std::move(future) };
        }

        bool _supportedInRequestCore(const ControlMethod method) noexcept
        {
            switch (method)
            {
            case ControlMethod::Ping:
            case ControlMethod::QueryTask:
            case ControlMethod::QueryAgents:
            case ControlMethod::AgentCommand:
            case ControlMethod::SubmitCommand:
            case ControlMethod::AcquireInputLease:
            case ControlMethod::ReleaseInputLease:
            case ControlMethod::RuntimeStatus:
            case ControlMethod::DrainRuntime:
                return true;
            case ControlMethod::SnapshotAndSubscribe:
            case ControlMethod::Unsubscribe:
            case ControlMethod::Count:
            default:
                return false;
            }
        }

    }

    namespace details
    {
        RuntimeClientSharedState::RuntimeClientSharedState(
            TaskRuntimeClientConfiguration configuration,
            TaskRuntimeClientDependencies dependencies) :
            Configuration{ std::move(configuration) },
            Dependencies{ std::move(dependencies) },
            Requests{ { _requestRegistryCapacity(Configuration), Configuration.MaximumTombstones } },
            Subscriptions{ { Configuration.MaximumSubscriptions } }
        {
            if (!Dependencies.Connector)
            {
                Dependencies.Connector = MakeRuntimeClientProductConnector(Configuration);
            }
            if (!Dependencies.RequestIds)
            {
                Dependencies.RequestIds = MakeRuntimeClientRequestIdSource();
            }
            if (!Dependencies.Backoff)
            {
                Dependencies.Backoff = MakeRuntimeClientBackoffPolicy();
            }
            if (!Dependencies.Scheduler)
            {
                Dependencies.Scheduler = MakeRuntimeClientScheduler();
            }
            HeartbeatRequestNamespace =
                RuntimeClientHeartbeatNamespace(Configuration.Hello.ClientInstanceId);
            try
            {
                SubscriptionBudget = std::make_shared<RuntimeClientSubscriptionAggregateBudget>(
                    Configuration.MaximumAggregateSubscriptionEventBytes);
            }
            catch (...)
            {
                SubscriptionBudget.reset();
            }
            IsValid = StopEvent && Requests.Valid() && Subscriptions.Valid() && SubscriptionBudget &&
                      SubscriptionBudget->Valid() && Dependencies.Connector &&
                      Dependencies.RequestIds && Dependencies.Backoff && Dependencies.Scheduler &&
                      HeartbeatRequestNamespace.has_value() &&
                      _validConfiguration(Configuration);
        }

        RuntimeClientSharedState::~RuntimeClientSharedState()
        {
            // The supervisor owns a shared reference to this state. If the public
            // facade is released from an injected connector/transport call, the
            // last reference is released by the supervisor itself after it exits.
            // Detach that already-finishing self-owned thread so std::thread's
            // destructor cannot terminate the process. The ordinary facade
            // destructor still joins through WaitForStopped().
            if (Supervisor && Supervisor->joinable())
            {
                if (Supervisor->get_id() == std::this_thread::get_id())
                {
                    DetachOrRetainRuntimeClientThread(Supervisor);
                }
                else
                {
                    StopEvent.Signal();
                    (void)WaitAndReleaseRuntimeClientThread(Supervisor);
                }
            }
        }

        bool RuntimeClientSharedState::Valid() const noexcept
        {
            return IsValid;
        }

        bool RuntimeClientSharedState::Start()
        {
            std::scoped_lock joinLock{ JoinMutex };
            std::scoped_lock lock{ Mutex };
            if (!IsValid || StartedOnce || ClientState != TaskRuntimeClientState::Stopped)
            {
                return false;
            }
            try
            {
                CompletionBuffer.reserve(Configuration.MaximumOperations);
                RetryBuffer.reserve(Configuration.MaximumOperations);
                RetiredBuffer.reserve(_requestRegistryCapacity(Configuration));
                SubscriptionDeliveryBuffer.reserve(Configuration.MaximumSubscriptions * 2);
                Operations.reserve(_requestRegistryCapacity(Configuration));
                SubscriptionDescriptors.reserve(Configuration.MaximumSubscriptions);
                IssuedRequestIds.reserve(Configuration.MaximumRememberedRequestIds);

                AdmissionOpen = true;
                StartedOnce = true;
                ClientState = TaskRuntimeClientState::Connecting;
                const auto state = shared_from_this();
                Supervisor = std::make_unique<std::thread>([state] { state->RunSupervisor(); });
                return true;
            }
            catch (...)
            {
                AdmissionOpen = false;
                StartedOnce = false;
                ClientState = TaskRuntimeClientState::Stopped;
                return false;
            }
        }

        RuntimeClientRequestHandle RuntimeClientSharedState::Submit(
            const ControlMethod method,
            std::string payloadJson,
            uint32_t maximumAttempts)
        {
            RuntimeClientRequestResult rejected;
            if (!IsValid || !_supportedInRequestCore(method))
            {
                rejected.Outcome = RuntimeClientRequestOutcome::InvalidRequest;
                rejected.Diagnostic = "Control method is not supported by the request core";
                return _readyHandle(std::move(rejected));
            }
            if (maximumAttempts == 0)
            {
                maximumAttempts = Configuration.DefaultMaximumAttempts;
            }
            if (maximumAttempts == 0 || maximumAttempts > Configuration.MaximumAttemptsPerOperation)
            {
                rejected.Outcome = RuntimeClientRequestOutcome::InvalidRequest;
                rejected.Diagnostic = "Request retry limit is invalid";
                return _readyHandle(std::move(rejected));
            }

            ControlRequest validation;
            validation.RequestId = ValidationRequestId;
            validation.Method = method;
            validation.PayloadJson = payloadJson;
            const auto encoded = EncodeControlRequest(validation);
            if (!encoded.Succeeded || encoded.Payload.size() > Configuration.MaximumRequestBytes)
            {
                rejected.Outcome = RuntimeClientRequestOutcome::InvalidRequest;
                rejected.Diagnostic = "Request payload is invalid or exceeds client bounds";
                return _readyHandle(std::move(rejected));
            }

            const auto generatedId = Dependencies.RequestIds->Next();
            if (!generatedId || !IsCanonicalProtocolUuid(*generatedId) ||
                IsReservedHeartbeatRequestId(*generatedId))
            {
                rejected.Outcome = RuntimeClientRequestOutcome::ProtocolError;
                rejected.Diagnostic = "Request ID source returned an invalid identifier";
                return _readyHandle(std::move(rejected));
            }

            auto promise = std::make_shared<std::promise<RuntimeClientRequestResult>>();
            auto future = promise->get_future().share();
            RuntimeClientOperationId operationId{};
            RuntimeClientRequestOutcome rejectedOutcome{ RuntimeClientRequestOutcome::ProtocolError };
            std::string rejectedDiagnostic;
            try
            {
                std::scoped_lock lock{ Mutex };
                if (!AdmissionOpen)
                {
                    rejectedOutcome = ClientState == TaskRuntimeClientState::Faulted ?
                                          RuntimeClientRequestOutcome::ProtocolError :
                                          RuntimeClientRequestOutcome::ClientStopped;
                    rejectedDiagnostic = "Task Runtime client is not accepting requests";
                }
                else if (UserOperationCount >= Configuration.MaximumOperations ||
                         encoded.Payload.size() >
                                 Configuration.MaximumQueuedBytes - Configuration.MaximumRequestBytes -
                                     Configuration.ReservedSubscriptionRequestBytes -
                                 std::min(
                                     UserQueuedBytes,
                                     Configuration.MaximumQueuedBytes - Configuration.MaximumRequestBytes -
                                         Configuration.ReservedSubscriptionRequestBytes))
                {
                    rejectedOutcome = RuntimeClientRequestOutcome::CapacityExceeded;
                    rejectedDiagnostic = "Task Runtime client request queue is full";
                }
                else if (NextOperationId == 0)
                {
                    AdmissionOpen = false;
                    ClientState = TaskRuntimeClientState::Faulted;
                    rejectedOutcome = RuntimeClientRequestOutcome::ProtocolError;
                    rejectedDiagnostic = "Client operation identifier space is exhausted";
                }
                else if (IssuedRequestIds.size() >= Configuration.MaximumRememberedRequestIds)
                {
                    rejectedOutcome = RuntimeClientRequestOutcome::CapacityExceeded;
                    rejectedDiagnostic = "Client request identifier history is full";
                }
                else if (!IssuedRequestIds.emplace(*generatedId).second)
                {
                    rejectedOutcome = RuntimeClientRequestOutcome::ProtocolError;
                    rejectedDiagnostic = "Request ID source reused an identifier";
                }
                else
                {
                    operationId = NextOperationId;
                    NextOperationId = NextOperationId == std::numeric_limits<RuntimeClientOperationId>::max() ?
                                          0 :
                                          NextOperationId + 1;

                    RuntimeClientLogicalOperation operation;
                    operation.OperationId = operationId;
                    operation.Method = method;
                    operation.PayloadJson = std::move(payloadJson);
                    operation.WireBytes = encoded.Payload.size();
                    operation.CurrentAttempt = 1;
                    operation.MaximumAttempts = maximumAttempts;
                    operation.CurrentRequestId = *generatedId;
                    operation.Promise = promise;

                    const auto add = Requests.Add(RuntimeClientRequestAttemptInfo{
                        operationId,
                        1,
                        *generatedId,
                        method,
                        0,
                        ClientRequestTransmission::NotSent,
                    });
                    if (add != RuntimeClientRequestAddStatus::Added)
                    {
                        operationId = 0;
                        rejectedOutcome = add == RuntimeClientRequestAddStatus::CapacityExceeded ?
                                              RuntimeClientRequestOutcome::CapacityExceeded :
                                              RuntimeClientRequestOutcome::ProtocolError;
                        rejectedDiagnostic = "Request registry rejected the generated attempt";
                    }
                    else
                    {
                        try
                        {
                            Operations.emplace(operation.OperationId, std::move(operation));
                            WriterQueue.emplace_back(RuntimeClientWriterItem{ operationId, 1, *generatedId });
                            QueuedBytes += encoded.Payload.size();
                            UserQueuedBytes += encoded.Payload.size();
                            ++UserOperationCount;
                        }
                        catch (...)
                        {
                            (void)Requests.CancelQueued(operationId);
                            Operations.erase(operationId);
                            operationId = 0;
                            rejectedOutcome = RuntimeClientRequestOutcome::CapacityExceeded;
                            rejectedDiagnostic = "Request queue allocation failed";
                        }
                    }
                }
            }
            catch (...)
            {
                operationId = 0;
                rejectedOutcome = RuntimeClientRequestOutcome::CapacityExceeded;
                rejectedDiagnostic = "Request state allocation failed";
            }

            if (operationId == 0)
            {
                RuntimeClientRequestResult result;
                result.Outcome = rejectedOutcome;
                result.Diagnostic = std::move(rejectedDiagnostic);
                promise->set_value(std::move(result));
                return { false, 0, std::move(future) };
            }

            WriterWake.notify_all();
            return { true, operationId, std::move(future) };
        }

        void RuntimeClientSharedState::RequestStop() noexcept
        {
            {
                std::scoped_lock lock{ Mutex };
                if (ClientState == TaskRuntimeClientState::Stopped ||
                    ClientState == TaskRuntimeClientState::Stopping)
                {
                    return;
                }
                AdmissionOpen = false;
                ClientState = TaskRuntimeClientState::Stopping;
            }
            StopEvent.Signal();
            WriterWake.notify_all();
        }

        bool RuntimeClientSharedState::WaitForStopped() noexcept
        {
            {
                std::scoped_lock lock{ Mutex };
                const auto current = std::this_thread::get_id();
                if (current == SupervisorThreadId || current == ReaderThreadId || current == WriterThreadId)
                {
                    return false;
                }
            }
            // Check for an internal caller before taking the join-serialization
            // mutex. An external waiter may already hold JoinMutex while joining
            // this very thread; taking it first here would deadlock a facade
            // destruction triggered from an injected transport call.
            std::scoped_lock joinLock{ JoinMutex };
            try
            {
                return WaitAndReleaseRuntimeClientThread(Supervisor);
            }
            catch (...)
            {
                return false;
            }
        }

        TaskRuntimeClientState RuntimeClientSharedState::State() const noexcept
        {
            std::scoped_lock lock{ Mutex };
            return ClientState;
        }

        RuntimeClientEpoch RuntimeClientSharedState::ActiveEpoch() const noexcept
        {
            std::scoped_lock lock{ Mutex };
            return Active ? Active->Epoch : 0;
        }

        std::string RuntimeClientSharedState::RuntimeInstanceId() const
        {
            std::scoped_lock lock{ Mutex };
            return CurrentRuntimeInstanceId;
        }

        ProtocolCapabilitySet RuntimeClientSharedState::NegotiatedCapabilities() const noexcept
        {
            std::scoped_lock lock{ Mutex };
            return CurrentCapabilities;
        }

        uint16_t RuntimeClientSharedState::ProtocolMinor() const noexcept
        {
            std::scoped_lock lock{ Mutex };
            return CurrentProtocolMinor;
        }
    }

    struct TaskRuntimeClient::Impl
    {
        std::shared_ptr<details::RuntimeClientSharedState> State;
    };

    TaskRuntimeClient::TaskRuntimeClient(
        TaskRuntimeClientConfiguration configuration,
        TaskRuntimeClientDependencies dependencies) :
        _impl{ std::make_shared<Impl>() }
    {
        _impl->State = std::make_shared<details::RuntimeClientSharedState>(
            std::move(configuration),
            std::move(dependencies));
    }

    TaskRuntimeClient::~TaskRuntimeClient()
    {
        RequestStop();
        (void)WaitForStopped();
    }

    bool TaskRuntimeClient::Valid() const noexcept
    {
        return _impl && _impl->State && _impl->State->Valid();
    }

    bool TaskRuntimeClient::Start()
    {
        return Valid() && _impl->State->Start();
    }

    RuntimeClientRequestHandle TaskRuntimeClient::Submit(
        const ControlMethod method,
        std::string payloadJson,
        const uint32_t maximumAttempts)
    {
        if (!Valid())
        {
            RuntimeClientRequestResult result;
            result.Outcome = RuntimeClientRequestOutcome::InvalidRequest;
            result.Diagnostic = "Task Runtime client configuration is invalid";
            return _readyHandle(std::move(result));
        }
        return _impl->State->Submit(method, std::move(payloadJson), maximumAttempts);
    }

    RuntimeClientSubscriptionHandle TaskRuntimeClient::Subscribe(RuntimeClientSubscribeOptions options)
    {
        return Valid() ? _impl->State->Subscribe(std::move(options)) :
                         RuntimeClientSubscriptionHandle{};
    }

    void TaskRuntimeClient::RequestStop() noexcept
    {
        if (_impl && _impl->State)
        {
            _impl->State->RequestStop();
        }
    }

    bool TaskRuntimeClient::WaitForStopped() noexcept
    {
        return !_impl || !_impl->State || _impl->State->WaitForStopped();
    }

    TaskRuntimeClientState TaskRuntimeClient::State() const noexcept
    {
        return _impl && _impl->State ?
                   _impl->State->State() :
                   TaskRuntimeClientState::Stopped;
    }

    RuntimeClientEpoch TaskRuntimeClient::ActiveEpoch() const noexcept
    {
        return _impl && _impl->State ? _impl->State->ActiveEpoch() : 0;
    }

    std::string TaskRuntimeClient::RuntimeInstanceId() const
    {
        return _impl && _impl->State ? _impl->State->RuntimeInstanceId() : std::string{};
    }

    ProtocolCapabilitySet TaskRuntimeClient::NegotiatedCapabilities() const noexcept
    {
        return _impl && _impl->State ? _impl->State->NegotiatedCapabilities() : 0;
    }

    uint16_t TaskRuntimeClient::ProtocolMinor() const noexcept
    {
        return _impl && _impl->State ? _impl->State->ProtocolMinor() : 0;
    }
}
