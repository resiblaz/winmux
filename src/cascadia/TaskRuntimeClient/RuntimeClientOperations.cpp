// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        RuntimeClientRequestOutcome _retiredOutcome(
            const ClientDisconnectAction action,
            const bool stopping) noexcept
        {
            if (action == ClientDisconnectAction::OutcomeUnknown)
            {
                return RuntimeClientRequestOutcome::OutcomeUnknown;
            }
            if (stopping)
            {
                return RuntimeClientRequestOutcome::ClientStopped;
            }
            switch (action)
            {
            case ClientDisconnectAction::Resnapshot:
                return RuntimeClientRequestOutcome::ResnapshotRequired;
            case ClientDisconnectAction::FailDisconnected:
            case ClientDisconnectAction::RetryWithNewRequestId:
                return RuntimeClientRequestOutcome::Disconnected;
            case ClientDisconnectAction::OutcomeUnknown:
            default:
                return RuntimeClientRequestOutcome::OutcomeUnknown;
            }
        }
    }

    std::optional<RuntimeClientPendingCompletion> RuntimeClientSharedState::CompleteOperationLocked(
        const RuntimeClientOperationId operationId,
        RuntimeClientRequestResult result)
    {
        const auto operation = Operations.find(operationId);
        if (operation == Operations.end())
        {
            return std::nullopt;
        }
        result.OperationId = operationId;
        result.Attempts = operation->second.CurrentAttempt;
        if (result.LastRequestId.empty())
        {
            result.LastRequestId = std::move(operation->second.CurrentRequestId);
        }
        if (result.Diagnostic.size() > Configuration.MaximumDiagnosticBytes)
        {
            result.Diagnostic.resize(Configuration.MaximumDiagnosticBytes);
        }

        RuntimeClientPendingCompletion completion;
        completion.Promise = std::move(operation->second.Promise);
        completion.Result = std::move(result);
        QueuedBytes = operation->second.WireBytes <= QueuedBytes ?
                          QueuedBytes - operation->second.WireBytes :
                          0;
        if (operation->second.Kind == RuntimeClientLogicalOperationKind::UserRequest &&
            UserOperationCount != 0)
        {
            --UserOperationCount;
            UserQueuedBytes = operation->second.WireBytes <= UserQueuedBytes ?
                                  UserQueuedBytes - operation->second.WireBytes :
                                  0;
        }
        Operations.erase(operation);
        return completion;
    }

    void RuntimeClientSharedState::ProcessRetiredLocked(
        std::vector<RuntimeClientRetiredRequest>& retired,
        const bool stopping,
        const bool protocolFault,
        const RuntimeTransportStatus transportStatus,
        const DWORD systemError,
        const std::string_view diagnostic,
        std::vector<RuntimeClientRetryWork>& retries,
        std::vector<RuntimeClientPendingCompletion>& completions)
    {
        for (auto& value : retired)
        {
            const auto operation = Operations.find(value.Attempt.OperationId);
            if (operation == Operations.end() ||
                operation->second.CurrentAttempt != value.Attempt.Attempt ||
                operation->second.CurrentRequestId != value.Attempt.RequestId)
            {
                continue;
            }

            if (operation->second.Kind != RuntimeClientLogicalOperationKind::UserRequest)
            {
                EraseSubscriptionOperationLocked(operation->first);
                continue;
            }

            if (!stopping && !protocolFault &&
                value.Action == ClientDisconnectAction::RetryWithNewRequestId &&
                operation->second.CurrentAttempt < operation->second.MaximumAttempts)
            {
                operation->second.Phase = RuntimeClientLogicalPhase::RetryPending;
                retries.emplace_back(RuntimeClientRetryWork{
                    operation->first,
                    operation->second.CurrentAttempt + 1,
                });
                continue;
            }

            RuntimeClientRequestResult result;
            result.Outcome = value.Action == ClientDisconnectAction::OutcomeUnknown ?
                                 RuntimeClientRequestOutcome::OutcomeUnknown :
                             protocolFault ?
                                 RuntimeClientRequestOutcome::ProtocolError :
                                 _retiredOutcome(value.Action, stopping);
            result.Epoch = value.Attempt.Epoch;
            result.LastRequestId = std::move(value.Attempt.RequestId);
            result.TransportStatus = transportStatus;
            result.SystemError = systemError;
            try
            {
                result.Diagnostic.assign(diagnostic.data(), diagnostic.size());
            }
            catch (...)
            {
                result.Diagnostic.clear();
            }
            if (auto completion = CompleteOperationLocked(operation->first, std::move(result)))
            {
                completions.emplace_back(std::move(*completion));
            }
        }
    }

    void RuntimeClientSharedState::QueueRetries(
        std::vector<RuntimeClientRetryWork>& retries,
        std::vector<RuntimeClientPendingCompletion>& completions) noexcept
    {
        for (const auto& retry : retries)
        {
            std::optional<std::string> requestId;
            try
            {
                requestId = Dependencies.RequestIds->Next();
            }
            catch (...)
            {
                requestId.reset();
            }

            std::optional<RuntimeClientWriterItem> writerItem;
            if (requestId)
            {
                try
                {
                    writerItem.emplace(RuntimeClientWriterItem{
                        retry.OperationId,
                        retry.NextAttempt,
                        *requestId,
                    });
                }
                catch (...)
                {
                    writerItem.reset();
                }
            }

            bool queued{};
            try
            {
                std::scoped_lock lock{ Mutex };
                const auto operation = Operations.find(retry.OperationId);
                if (operation == Operations.end() ||
                    operation->second.Phase != RuntimeClientLogicalPhase::RetryPending ||
                    retry.NextAttempt != operation->second.CurrentAttempt + 1)
                {
                    continue;
                }
                if (!AdmissionOpen)
                {
                    RuntimeClientRequestResult result;
                    result.Outcome = RuntimeClientRequestOutcome::ClientStopped;
                    result.Diagnostic = "Client stopped before retry could be queued";
                    if (auto completion = CompleteOperationLocked(operation->first, std::move(result)))
                    {
                        completions.emplace_back(std::move(*completion));
                    }
                    continue;
                }
                if (!requestId || !writerItem || !IsCanonicalProtocolUuid(*requestId))
                {
                    RuntimeClientRequestResult result;
                    result.Outcome = RuntimeClientRequestOutcome::ProtocolError;
                    result.Diagnostic = "Request ID source failed while retrying";
                    if (auto completion = CompleteOperationLocked(operation->first, std::move(result)))
                    {
                        completions.emplace_back(std::move(*completion));
                    }
                    continue;
                }
                if (IssuedRequestIds.size() >= Configuration.MaximumRememberedRequestIds)
                {
                    RuntimeClientRequestResult result;
                    result.Outcome = RuntimeClientRequestOutcome::CapacityExceeded;
                    result.Diagnostic = "Client request identifier history is full";
                    if (auto completion = CompleteOperationLocked(operation->first, std::move(result)))
                    {
                        completions.emplace_back(std::move(*completion));
                    }
                    continue;
                }
                if (!IssuedRequestIds.emplace(*requestId).second)
                {
                    RuntimeClientRequestResult result;
                    result.Outcome = RuntimeClientRequestOutcome::ProtocolError;
                    result.Diagnostic = "Request ID source reused an identifier while retrying";
                    if (auto completion = CompleteOperationLocked(operation->first, std::move(result)))
                    {
                        completions.emplace_back(std::move(*completion));
                    }
                    continue;
                }

                const auto add = Requests.Add(RuntimeClientRequestAttemptInfo{
                    operation->first,
                    retry.NextAttempt,
                    *requestId,
                    operation->second.Method,
                    0,
                    ClientRequestTransmission::NotSent,
                });
                if (add != RuntimeClientRequestAddStatus::Added)
                {
                    RuntimeClientRequestResult result;
                    result.Outcome = add == RuntimeClientRequestAddStatus::CapacityExceeded ?
                                         RuntimeClientRequestOutcome::CapacityExceeded :
                                         RuntimeClientRequestOutcome::ProtocolError;
                    result.Diagnostic = "Request registry rejected a retry attempt";
                    if (auto completion = CompleteOperationLocked(operation->first, std::move(result)))
                    {
                        completions.emplace_back(std::move(*completion));
                    }
                    continue;
                }

                try
                {
                    WriterQueue.emplace_back(std::move(*writerItem));
                }
                catch (...)
                {
                    (void)Requests.CancelQueued(operation->first);
                    RuntimeClientRequestResult result;
                    result.Outcome = RuntimeClientRequestOutcome::CapacityExceeded;
                    result.Diagnostic = "Retry queue allocation failed";
                    if (auto completion = CompleteOperationLocked(operation->first, std::move(result)))
                    {
                        completions.emplace_back(std::move(*completion));
                    }
                    continue;
                }
                operation->second.CurrentAttempt = retry.NextAttempt;
                operation->second.CurrentRequestId = std::move(*requestId);
                operation->second.Phase = RuntimeClientLogicalPhase::AttemptQueued;
                queued = true;
            }
            catch (...)
            {
                try
                {
                    std::scoped_lock lock{ Mutex };
                    RuntimeClientRequestResult result;
                    result.Outcome = RuntimeClientRequestOutcome::ProtocolError;
                    result.Diagnostic = "Unexpected retry state failure";
                    if (auto completion = CompleteOperationLocked(retry.OperationId, std::move(result)))
                    {
                        completions.emplace_back(std::move(*completion));
                    }
                }
                catch (...)
                {
                }
            }
            if (queued)
            {
                WriterWake.notify_all();
            }
        }
        retries.clear();
    }

    void RuntimeClientSharedState::FinishStopped(
        std::vector<RuntimeClientPendingCompletion>& completions) noexcept
    {
        std::shared_ptr<RuntimeClientEpochContext> active;
        try
        {
            std::scoped_lock lock{ Mutex };
            for (auto iterator = Operations.begin(); iterator != Operations.end();)
            {
                const auto operationId = iterator->first;
                ++iterator;
                RuntimeClientRequestResult result;
                result.Outcome = RuntimeClientRequestOutcome::ClientStopped;
                const auto operation = Operations.find(operationId);
                if (operation != Operations.end() &&
                    operation->second.Kind != RuntimeClientLogicalOperationKind::UserRequest)
                {
                    EraseSubscriptionOperationLocked(operationId);
                }
                else if (auto completion = CompleteOperationLocked(operationId, std::move(result)))
                {
                    completions.emplace_back(std::move(*completion));
                }
            }
            FailSubscriptionsLocked(
                RuntimeClientSubscriptionEventKind::ClientStopped,
                RuntimeTransportStatus::Cancelled,
                ERROR_OPERATION_ABORTED,
                "Task Runtime client stopped",
                SubscriptionDeliveryBuffer);
            WriterQueue.clear();
            active = std::move(Active);
            AdmissionOpen = false;
            ClientState = TaskRuntimeClientState::Stopped;
            SupervisorThreadId = {};
            ReaderThreadId = {};
            WriterThreadId = {};
        }
        catch (...)
        {
            std::scoped_lock lock{ Mutex };
            AdmissionOpen = false;
            ClientState = TaskRuntimeClientState::Stopped;
        }
        if (active)
        {
            active->Cancel();
            active->Close();
        }
    }

    void RuntimeClientSharedState::FailAllLocked(
        const RuntimeClientRequestOutcome outcome,
        const RuntimeTransportStatus transportStatus,
        const DWORD systemError,
        const std::string_view diagnostic,
        std::vector<RuntimeClientPendingCompletion>& completions)
    {
        for (auto iterator = Operations.begin(); iterator != Operations.end();)
        {
            const auto operationId = iterator->first;
            ++iterator;
            RuntimeClientRequestResult result;
            result.Outcome = outcome;
            result.TransportStatus = transportStatus;
            result.SystemError = systemError;
            try
            {
                result.Diagnostic.assign(diagnostic.data(), diagnostic.size());
            }
            catch (...)
            {
                result.Diagnostic.clear();
            }
            const auto operation = Operations.find(operationId);
            if (operation != Operations.end() &&
                operation->second.Kind != RuntimeClientLogicalOperationKind::UserRequest)
            {
                EraseSubscriptionOperationLocked(operationId);
            }
            else if (auto completion = CompleteOperationLocked(operationId, std::move(result)))
            {
                completions.emplace_back(std::move(*completion));
            }
        }
        WriterQueue.clear();
    }

    void RuntimeClientSharedState::Deliver(
        std::vector<RuntimeClientPendingCompletion>& completions) noexcept
    {
        for (auto& completion : completions)
        {
            if (!completion.Promise)
            {
                continue;
            }
            try
            {
                completion.Promise->set_value(std::move(completion.Result));
            }
            catch (...)
            {
            }
        }
        completions.clear();
    }
}
