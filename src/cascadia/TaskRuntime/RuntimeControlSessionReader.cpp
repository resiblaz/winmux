// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlSessionState.h"

#include "RuntimeControlHandshake.h"

#include "ProtocolRpcJson.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        template<typename TCallback>
        class ScopeExit final
        {
        public:
            explicit ScopeExit(TCallback callback) :
                _callback{ std::move(callback) }
            {
            }

            ~ScopeExit() noexcept
            {
                if (_active)
                {
                    _callback();
                }
            }

            ScopeExit(const ScopeExit&) = delete;
            ScopeExit& operator=(const ScopeExit&) = delete;

            void Release() noexcept
            {
                _active = false;
            }

        private:
            TCallback _callback;
            bool _active{ true };
        };

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

    void RuntimeControlSession::Impl::CancelPlannedSubscription(
        std::optional<RuntimeControlSubscriptionPlan>& plannedSubscription,
        const bool transportReservationInstalled) noexcept
    {
        if (!plannedSubscription)
        {
            return;
        }
        auto cancelled = std::move(*plannedSubscription);
        plannedSubscription.reset();
        if (transportReservationInstalled && SubscriptionOwnershipMutex)
        {
            std::scoped_lock ownershipLock{ *SubscriptionOwnershipMutex };
            Subscriptions.CancelPlan(cancelled);
        }
        if (Router)
        {
            Router->CancelSubscriptionPlan(Client, cancelled);
        }
    }

    bool RuntimeControlSession::Impl::EnqueueResponse(
        const ControlResponse& response,
        const bool completeRequest,
        const ControlMethod method,
        std::optional<RuntimeControlSubscriptionChange> subscriptionChange,
        std::optional<RuntimeControlSubscriptionPlan> plannedSubscription)
    {
        ScopeExit plannedSubscriptionCleanup{ [&]() noexcept {
            CancelPlannedSubscription(plannedSubscription, true);
        } };
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

        if (!subscriptionChange && plannedSubscription)
        {
            CancelPlannedSubscription(plannedSubscription, true);
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
            CancelPlannedSubscription(plannedSubscription, true);
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
                CancelPlannedSubscription(plannedSubscription, true);
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
                plannedSubscription.reset();
                plannedSubscriptionCleanup.Release();
                return true;
            }
            CancelPlannedSubscription(plannedSubscription, true);
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
                Configuration.HandshakeTimeoutMilliseconds,
                CancellationEvent);
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
                const auto received = Channel.Receive(
                    Configuration.ReadTimeoutMilliseconds,
                    CancellationEvent);
                if (!received.Succeeded())
                {
                    RequestStop(_readCloseReason(received.Status));
                    break;
                }
                if (cancellation.IsCancellationRequested())
                {
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
                if (cancellation.IsCancellationRequested())
                {
                    break;
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
                bool subscriptionReservationInstalled{};
                ScopeExit plannedSubscriptionCleanup{ [&]() noexcept {
                    CancelPlannedSubscription(
                        plannedSubscription,
                        subscriptionReservationInstalled);
                } };
                if (decoded.Value.Method == ControlMethod::SnapshotAndSubscribe)
                {
                    if (cancellation.IsCancellationRequested())
                    {
                        break;
                    }
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
                    if (cancellation.IsCancellationRequested())
                    {
                        CancelPlannedSubscription(plannedSubscription, false);
                        break;
                    }

                    auto planStatus = RuntimeControlSubscriptionPlanStatus::Invalid;
                    if (plannedSubscription && SubscriptionOwnershipMutex)
                    {
                        std::scoped_lock ownershipLock{ *SubscriptionOwnershipMutex };
                        if (!cancellation.IsCancellationRequested())
                        {
                            planStatus = Subscriptions.Plan(*plannedSubscription);
                        }
                    }
                    if (cancellation.IsCancellationRequested())
                    {
                        CancelPlannedSubscription(
                            plannedSubscription,
                            planStatus == RuntimeControlSubscriptionPlanStatus::Accepted);
                        break;
                    }
                    if (planStatus != RuntimeControlSubscriptionPlanStatus::Accepted)
                    {
                        const auto code = planStatus == RuntimeControlSubscriptionPlanStatus::Conflict ?
                                              ProtocolRpcErrorCode::Conflict :
                                          planStatus == RuntimeControlSubscriptionPlanStatus::LimitReached ?
                                              ProtocolRpcErrorCode::TooManyRequests :
                                              ProtocolRpcErrorCode::InternalError;
                        CancelPlannedSubscription(plannedSubscription, false);
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
                    subscriptionReservationInstalled = true;
                }

                if (cancellation.IsCancellationRequested())
                {
                    CancelPlannedSubscription(
                        plannedSubscription,
                        subscriptionReservationInstalled);
                    break;
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
                    CancelPlannedSubscription(
                        plannedSubscription,
                        subscriptionReservationInstalled);
                    break;
                }

                ControlResponse response;
                response.RequestId = decoded.Value.RequestId;
                response.Succeeded = routed.Succeeded;
                response.ResultJson = std::move(routed.ResultJson);
                response.Error = std::move(routed.Error);
                auto responsePlan = std::move(plannedSubscription);
                plannedSubscription.reset();
                plannedSubscriptionCleanup.Release();
                if (!EnqueueResponse(
                        response,
                        true,
                        decoded.Value.Method,
                        std::move(routed.SubscriptionChange),
                        std::move(responsePlan)))
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
