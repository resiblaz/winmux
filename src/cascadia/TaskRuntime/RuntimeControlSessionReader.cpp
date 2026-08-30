// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlSessionState.h"

#include "ProtocolRpcJson.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        ControlResponse _errorResponse(
            const std::string_view requestId,
            const ProtocolRpcErrorCode code,
            std::string message)
        {
            ControlResponse response;
            response.RequestId = requestId;
            response.Error = ProtocolRpcError{ code, std::move(message), "{}" };
            return response;
        }

        RuntimeControlSessionCloseReason _readCloseReason(const RuntimeTransportStatus status) noexcept
        {
            switch (status)
            {
            case RuntimeTransportStatus::Timeout:
                return RuntimeControlSessionCloseReason::IdleTimeout;
            case RuntimeTransportStatus::Disconnected:
                return RuntimeControlSessionCloseReason::PeerDisconnected;
            case RuntimeTransportStatus::MalformedFrame:
                return RuntimeControlSessionCloseReason::MalformedRequest;
            default:
                return RuntimeControlSessionCloseReason::TransportError;
            }
        }
    }

    bool RuntimeControlSession::Impl::EnqueueResponse(
        const ControlResponse& response,
        const bool completeRequest,
        const ControlMethod method,
        std::optional<RuntimeControlSubscriptionChange> subscriptionChange,
        std::optional<RuntimeControlSubscriptionPlan> plannedSubscription)
    {
        auto effectiveResponse = response;
        const auto validSubscriptionEffect = [&] {
            if (!response.Succeeded)
            {
                return !subscriptionChange;
            }
            if (method == ControlMethod::SnapshotAndSubscribe)
            {
                return plannedSubscription && subscriptionChange &&
                       subscriptionChange->Action == RuntimeControlSubscriptionAction::Bind &&
                       IsValidRuntimeControlSubscriptionChange(method, *subscriptionChange) &&
                       subscriptionChange->SubscriptionId == plannedSubscription->SubscriptionId &&
                       subscriptionChange->Generation == plannedSubscription->Generation;
            }
            if (method == ControlMethod::Unsubscribe)
            {
                return subscriptionChange &&
                       subscriptionChange->Action == RuntimeControlSubscriptionAction::Unbind &&
                       IsValidRuntimeControlSubscriptionChange(method, *subscriptionChange);
            }
            return !subscriptionChange;
        }();
        if (!validSubscriptionEffect)
        {
            effectiveResponse = _errorResponse(
                response.RequestId,
                ProtocolRpcErrorCode::InternalError,
                "Runtime router returned an invalid subscription change");
            subscriptionChange.reset();
        }

        const auto cancelPlan = [&] {
            if (plannedSubscription && SubscriptionOwnershipMutex)
            {
                const auto cancelled = *plannedSubscription;
                {
                    std::scoped_lock ownershipLock{ *SubscriptionOwnershipMutex };
                    Subscriptions.CancelPlan(cancelled);
                }
                plannedSubscription.reset();
                Router->CancelSubscriptionPlan(Client, cancelled);
            }
        };
        if (!subscriptionChange && plannedSubscription)
        {
            cancelPlan();
        }

        auto encoded = EncodeControlResponse(effectiveResponse);
        if (!encoded.Succeeded ||
            encoded.Payload.size() > MaximumPayloadBytes.load(std::memory_order_acquire))
        {
            encoded = EncodeControlResponse(_errorResponse(
                response.RequestId,
                ProtocolRpcErrorCode::InternalError,
                "Runtime router returned an invalid response"));
            subscriptionChange.reset();
            cancelPlan();
        }
        if (!encoded.Succeeded ||
            encoded.Payload.size() > MaximumPayloadBytes.load(std::memory_order_acquire))
        {
            RequestStop(RuntimeControlSessionCloseReason::TransportError);
            return false;
        }

        if (subscriptionChange)
        {
            if (!SubscriptionOwnershipMutex)
            {
                RequestStop(RuntimeControlSessionCloseReason::TransportError);
                return false;
            }
            RuntimeControlSubscriptionApplyStatus applied;
            {
                std::scoped_lock ownershipLock{ *SubscriptionOwnershipMutex };
                applied = Subscriptions.EnqueueResponseAndApply(
                    Outbound,
                    std::move(encoded.Payload),
                    completeRequest ? std::optional{ response.RequestId } : std::nullopt,
                    *subscriptionChange);
            }
            if (applied == RuntimeControlSubscriptionApplyStatus::Accepted)
            {
                return true;
            }
            RequestStop(applied == RuntimeControlSubscriptionApplyStatus::QueueLimitReached ?
                            RuntimeControlSessionCloseReason::OutboundQueueLimit :
                            RuntimeControlSessionCloseReason::TransportError);
            return false;
        }

        const auto queued = Outbound.Enqueue(
            std::move(encoded.Payload),
            completeRequest ? std::optional{ response.RequestId } : std::nullopt);
        if (queued == RuntimeControlQueueStatus::Accepted)
        {
            return true;
        }
        RequestStop(queued == RuntimeControlQueueStatus::QueueLimitReached ?
                        RuntimeControlSessionCloseReason::OutboundQueueLimit :
                        RuntimeControlSessionCloseReason::TransportError);
        return false;
    }

    void RuntimeControlSession::Impl::RunReader() noexcept
    {
        try
        {
            const auto handshake = PerformRuntimeServerHandshake(
                Channel,
                ProtocolSession,
                Configuration.HandshakeTimeoutMilliseconds);
            if (!handshake.Succeeded())
            {
                RequestStop(handshake.Status == RuntimeTransportStatus::ProtocolRejected ?
                                RuntimeControlSessionCloseReason::HandshakeRejected :
                                _readCloseReason(handshake.Status));
                ReaderDone.store(true, std::memory_order_release);
                return;
            }

            {
                std::scoped_lock clientLock{ ClientMutex };
                Client.ClientInstanceId = handshake.Request.ClientInstanceId;
                Client.ClientKind = handshake.Request.ClientKind;
                Client.NegotiatedCapabilities = handshake.Decision.Response.Capabilities;
            }
            MaximumPayloadBytes.store(
                handshake.Decision.Response.MaximumControlFrameBytes,
                std::memory_order_release);
            if (!Subscriptions.ConfigureMaximumPayloadBytes(
                    handshake.Decision.Response.MaximumControlFrameBytes))
            {
                RequestStop(RuntimeControlSessionCloseReason::TransportError);
                ReaderDone.store(true, std::memory_order_release);
                return;
            }
            const RuntimeControlCancellationToken cancellation{ CancellationState };

            while (!cancellation.IsCancellationRequested())
            {
                const auto received = Channel.Receive(Configuration.ReadTimeoutMilliseconds);
                if (!received.Succeeded())
                {
                    RequestStop(_readCloseReason(received.Status));
                    break;
                }
                const auto decoded = DecodeControlRequest(received.Payload);
                if (!decoded.Succeeded())
                {
                    RequestStop(RuntimeControlSessionCloseReason::MalformedRequest);
                    break;
                }

                ControlSessionDecision decision;
                {
                    std::scoped_lock lock{ ProtocolMutex };
                    decision = ProtocolSession.BeginRequest(decoded.Value);
                }
                if (!decision.Accepted)
                {
                    if (!EnqueueResponse(
                            _errorResponse(decoded.Value.RequestId, decision.Error, std::move(decision.Message)),
                            false))
                    {
                        break;
                    }
                    continue;
                }

                std::optional<RuntimeControlSubscriptionPlan> plannedSubscription;
                if (decoded.Value.Method == ControlMethod::SnapshotAndSubscribe)
                {
                    try
                    {
                        plannedSubscription = Router->PlanSubscription(
                            Client,
                            decoded.Value,
                            cancellation);
                    }
                    catch (...)
                    {
                        plannedSubscription.reset();
                    }

                    auto planStatus = RuntimeControlSubscriptionPlanStatus::Invalid;
                    if (plannedSubscription && SubscriptionOwnershipMutex)
                    {
                        std::scoped_lock ownershipLock{ *SubscriptionOwnershipMutex };
                        planStatus = Subscriptions.Plan(*plannedSubscription);
                    }
                    if (planStatus != RuntimeControlSubscriptionPlanStatus::Accepted)
                    {
                        const auto code = planStatus == RuntimeControlSubscriptionPlanStatus::Conflict ?
                                              ProtocolRpcErrorCode::Conflict :
                                          planStatus == RuntimeControlSubscriptionPlanStatus::LimitReached ?
                                              ProtocolRpcErrorCode::TooManyRequests :
                                              ProtocolRpcErrorCode::InternalError;
                        if (!EnqueueResponse(
                                _errorResponse(
                                    decoded.Value.RequestId,
                                    code,
                                    "Runtime could not plan the subscription"),
                                true,
                                decoded.Value.Method))
                        {
                            break;
                        }
                        continue;
                    }
                }

                RuntimeControlRouteResult routed;
                try
                {
                    routed = Router->Route(Client, decoded.Value, cancellation);
                }
                catch (...)
                {
                    routed.Error = ProtocolRpcError{
                        ProtocolRpcErrorCode::InternalError,
                        "Runtime router failed",
                        "{}"
                    };
                }
                if (cancellation.IsCancellationRequested())
                {
                    if (plannedSubscription && SubscriptionOwnershipMutex)
                    {
                        const auto cancelled = *plannedSubscription;
                        {
                            std::scoped_lock ownershipLock{ *SubscriptionOwnershipMutex };
                            Subscriptions.CancelPlan(cancelled);
                        }
                        Router->CancelSubscriptionPlan(Client, cancelled);
                    }
                    break;
                }

                ControlResponse response;
                response.RequestId = decoded.Value.RequestId;
                response.Succeeded = routed.Succeeded;
                response.ResultJson = std::move(routed.ResultJson);
                response.Error = std::move(routed.Error);
                if (!EnqueueResponse(
                        response,
                        true,
                        decoded.Value.Method,
                        std::move(routed.SubscriptionChange),
                        std::move(plannedSubscription)))
                {
                    break;
                }
            }
        }
        catch (...)
        {
            RequestStop(RuntimeControlSessionCloseReason::TransportError);
        }
        ReaderDone.store(true, std::memory_order_release);
    }
}
