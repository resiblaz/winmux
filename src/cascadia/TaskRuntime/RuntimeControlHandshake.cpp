// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlChannel.h"

#include "ControlSession.h"
#include "ProtocolJson.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        uint32_t _remaining(const uint64_t deadline) noexcept
        {
            const auto now = GetTickCount64();
            return now >= deadline ? 0 : static_cast<uint32_t>(deadline - now);
        }

        template<typename TResult>
        bool _requireRemaining(const uint64_t deadline, TResult& result)
        {
            if (_remaining(deadline) != 0)
            {
                return true;
            }
            result.Status = RuntimeTransportStatus::Timeout;
            result.Error = "Runtime handshake timed out";
            return false;
        }
    }

    RuntimeClientHandshakeResult PerformRuntimeClientHandshake(
        RuntimeControlChannel& channel,
        const HelloRequest& request,
        const uint32_t timeoutMilliseconds)
    {
        RuntimeClientHandshakeResult result;
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        const auto encoded = EncodeHelloRequest(request);
        if (!encoded.Succeeded)
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = encoded.Error;
            return result;
        }
        if (!_requireRemaining(deadline, result))
        {
            return result;
        }
        const auto sent = channel.Send(encoded.Payload, _remaining(deadline));
        if (!sent.Succeeded())
        {
            static_cast<RuntimeTransportResult&>(result) = sent;
            return result;
        }
        if (!_requireRemaining(deadline, result))
        {
            return result;
        }
        const auto received = channel.Receive(_remaining(deadline));
        if (!received.Succeeded())
        {
            static_cast<RuntimeTransportResult&>(result) = received;
            return result;
        }
        const auto decoded = DecodeHelloResponse(received.Payload);
        if (!decoded.Succeeded())
        {
            result.Status = RuntimeTransportStatus::MalformedFrame;
            result.Error = decoded.Error;
            return result;
        }
        const auto decision = ValidateHelloResponse(request, decoded.Value);
        result.HandshakeError = decision.Error;
        if (!decision.Accepted)
        {
            result.Status = RuntimeTransportStatus::ProtocolRejected;
            result.Error = decision.Message;
            return result;
        }
        if (!channel.ConfigureMaximumPayloadBytes(decision.Response.MaximumControlFrameBytes))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Negotiated control payload limit could not be applied";
            return result;
        }
        result.Status = RuntimeTransportStatus::Succeeded;
        result.Response = decision.Response;
        return result;
    }

    RuntimeServerHandshakeResult PerformRuntimeServerHandshake(
        RuntimeControlChannel& channel,
        const RuntimeHandshakeConfiguration& runtime,
        const uint32_t timeoutMilliseconds)
    {
        ControlSession session{ runtime };
        return PerformRuntimeServerHandshake(channel, session, timeoutMilliseconds);
    }

    RuntimeServerHandshakeResult PerformRuntimeServerHandshake(
        RuntimeControlChannel& channel,
        ControlSession& session,
        const uint32_t timeoutMilliseconds)
    {
        RuntimeServerHandshakeResult result;
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        if (!_requireRemaining(deadline, result))
        {
            return result;
        }
        const auto received = channel.Receive(_remaining(deadline));
        if (!received.Succeeded())
        {
            static_cast<RuntimeTransportResult&>(result) = received;
            return result;
        }
        const auto decoded = DecodeHelloRequest(received.Payload);
        if (!decoded.Succeeded())
        {
            result.Status = RuntimeTransportStatus::MalformedFrame;
            result.Error = decoded.Error;
            return result;
        }
        result.Request = decoded.Value;
        result.Decision = session.AcceptHello(result.Request);
        if (!result.Decision.Accepted)
        {
            result.Status = RuntimeTransportStatus::ProtocolRejected;
            result.Error = result.Decision.Message;
            return result;
        }
        const auto encoded = EncodeHelloResponse(result.Decision.Response);
        if (!encoded.Succeeded)
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = encoded.Error;
            return result;
        }
        if (!_requireRemaining(deadline, result))
        {
            return result;
        }
        const auto sent = channel.Send(encoded.Payload, _remaining(deadline));
        if (!sent.Succeeded())
        {
            static_cast<RuntimeTransportResult&>(result) = sent;
            return result;
        }
        if (!channel.ConfigureMaximumPayloadBytes(result.Decision.Response.MaximumControlFrameBytes))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Negotiated control payload limit could not be applied";
            return result;
        }
        result.Status = RuntimeTransportStatus::Succeeded;
        return result;
    }
}
