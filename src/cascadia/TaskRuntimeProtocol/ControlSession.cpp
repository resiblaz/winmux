// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ControlSession.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        ControlSessionDecision _reject(const ProtocolRpcErrorCode error, std::string message)
        {
            ControlSessionDecision decision;
            decision.Error = error;
            decision.Message = std::move(message);
            return decision;
        }
    }

    ControlSession::ControlSession(
        RuntimeHandshakeConfiguration runtime,
        const size_t maximumInflightRequests) :
        _runtime{ std::move(runtime) },
        _maximumInflightRequests{ maximumInflightRequests }
    {
    }

    HandshakeDecision ControlSession::AcceptHello(const HelloRequest& request)
    {
        if (_state != ControlSessionState::AwaitingHello)
        {
            HandshakeDecision decision;
            decision.Error = HandshakeError::InvalidRequest;
            decision.Message = "Hello is only valid as the first control message";
            _state = ControlSessionState::Closing;
            return decision;
        }
        if (_maximumInflightRequests == 0 ||
            _maximumInflightRequests > AbsoluteMaximumInflightRequests)
        {
            HandshakeDecision decision;
            decision.Error = HandshakeError::InvalidRuntimeConfiguration;
            decision.Message = "Control session request bound is invalid";
            _state = ControlSessionState::Closing;
            return decision;
        }

        auto decision = NegotiateHandshake(request, _runtime);
        if (!decision.Accepted)
        {
            _state = ControlSessionState::Closing;
            return decision;
        }
        _negotiatedCapabilities = decision.Response.Capabilities;
        _state = ControlSessionState::Ready;
        return decision;
    }

    ControlSessionDecision ControlSession::BeginRequest(const ControlRequest& request)
    {
        if (_state == ControlSessionState::AwaitingHello)
        {
            return _reject(ProtocolRpcErrorCode::HandshakeRequired, "Hello must complete before requests");
        }
        if (_state != ControlSessionState::Ready)
        {
            return _reject(ProtocolRpcErrorCode::RuntimeUnavailable, "Control session is closing or closed");
        }
        if (!IsCanonicalProtocolUuid(request.RequestId) || ControlMethodName(request.Method).empty() ||
            request.PayloadJson.empty() || request.PayloadJson.size() > MaximumControlFrameBytes ||
            !IsValidUtf8(request.PayloadJson))
        {
            return _reject(ProtocolRpcErrorCode::InvalidRequest, "Control request identity or method is invalid");
        }
        const auto required = RequiredCapabilities(request.Method);
        if ((required & _negotiatedCapabilities) != required)
        {
            return _reject(ProtocolRpcErrorCode::CapabilityRequired, "Control method capability was not negotiated");
        }
        if (_inflightRequests.contains(request.RequestId))
        {
            return _reject(ProtocolRpcErrorCode::DuplicateRequest, "Control request ID is already in flight");
        }
        if (_inflightRequests.size() >= _maximumInflightRequests)
        {
            return _reject(ProtocolRpcErrorCode::TooManyRequests, "Control session in-flight request limit reached");
        }

        _inflightRequests.emplace(request.RequestId);
        ControlSessionDecision decision;
        decision.Accepted = true;
        return decision;
    }

    bool ControlSession::CompleteRequest(const std::string_view requestId)
    {
        if (!IsCanonicalProtocolUuid(requestId))
        {
            return false;
        }
        return _inflightRequests.erase(std::string{ requestId }) == 1;
    }

    void ControlSession::BeginClose() noexcept
    {
        if (_state != ControlSessionState::Closed)
        {
            _state = ControlSessionState::Closing;
        }
    }

    void ControlSession::MarkClosed() noexcept
    {
        _inflightRequests.clear();
        _state = ControlSessionState::Closed;
    }

    ControlSessionState ControlSession::State() const noexcept
    {
        return _state;
    }

    size_t ControlSession::InflightRequestCount() const noexcept
    {
        return _inflightRequests.size();
    }

    ProtocolCapabilitySet ControlSession::NegotiatedCapabilities() const noexcept
    {
        return _negotiatedCapabilities;
    }
}
