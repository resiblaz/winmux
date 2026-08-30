// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalHandshake.h"

#include "ProtocolCapabilities.h"
#include "ProtocolJson.h"
#include "ProtocolValidation.h"

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
            result.SystemError = WAIT_TIMEOUT;
            result.Error = "Runtime terminal handshake timed out";
            return false;
        }

        bool _requiresTerminalStream(const HelloRequest& request) noexcept
        {
            return HasCapability(
                request.RequiredCapabilities,
                ProtocolCapability::TerminalStream);
        }

        std::vector<uint8_t> _payload(const std::string_view value)
        {
            std::vector<uint8_t> result;
            result.reserve(value.size());
            for (const auto character : value)
            {
                result.push_back(static_cast<uint8_t>(
                    static_cast<unsigned char>(character)));
            }
            return result;
        }

        std::string _text(const std::vector<uint8_t>& value)
        {
            std::string result;
            result.reserve(value.size());
            for (const auto byte : value)
            {
                result.push_back(static_cast<char>(byte));
            }
            return result;
        }

        void _sendRejection(
            RuntimeTerminalChannel& channel,
            const uint64_t deadline,
            const HANDLE cancellationEvent) noexcept
        {
            try
            {
                const auto remaining = _remaining(deadline);
                if (remaining == 0)
                {
                    return;
                }
                TerminalFrame error;
                error.Kind = TerminalFrameKind::Error;
                error.Sequence = 1;
                error.Generation = 0;
                error.Payload = _payload("terminal hello rejected");
                (void)channel.Send(error, remaining, cancellationEvent);
            }
            catch (...)
            {
            }
        }
    }

    RuntimeTerminalClientHandshakeResult PerformRuntimeTerminalClientHandshake(
        RuntimeTerminalChannel& channel,
        const HelloRequest& request,
        const std::string_view expectedRuntimeInstanceId,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        RuntimeTerminalClientHandshakeResult result;
        const auto endpoint = channel.Endpoint();
        if (!channel || !endpoint ||
            request.ExpectedRuntimeIdentity != endpoint->RuntimeIdentity ||
            !IsCanonicalProtocolUuid(expectedRuntimeInstanceId) ||
            !_requiresTerminalStream(request))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error =
                "Terminal Hello does not match its endpoint or required capability";
            return result;
        }
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        const auto encoded = EncodeHelloRequest(request);
        if (!encoded.Succeeded)
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = encoded.Error;
            return result;
        }

        TerminalFrame hello;
        hello.Kind = TerminalFrameKind::Hello;
        hello.Sequence = 1;
        hello.Generation = 0;
        hello.Payload = _payload(encoded.Payload);
        if (!_requireRemaining(deadline, result))
        {
            return result;
        }
        const auto sent = channel.Send(
            hello,
            _remaining(deadline),
            cancellationEvent);
        if (!sent.Succeeded())
        {
            static_cast<RuntimeTransportResult&>(result) = sent;
            return result;
        }
        if (!_requireRemaining(deadline, result))
        {
            return result;
        }
        const auto received = channel.Receive(
            _remaining(deadline),
            cancellationEvent);
        if (!received.Succeeded())
        {
            static_cast<RuntimeTransportResult&>(result) = received;
            return result;
        }
        if (received.Frame.Sequence != 1)
        {
            result.Status = RuntimeTransportStatus::MalformedFrame;
            result.Error = "Terminal Hello response sequence is invalid";
            return result;
        }
        if (received.Frame.Kind == TerminalFrameKind::Error)
        {
            result.Status = RuntimeTransportStatus::ProtocolRejected;
            result.Error = _text(received.Frame.Payload);
            return result;
        }
        if (received.Frame.Kind != TerminalFrameKind::Hello)
        {
            result.Status = RuntimeTransportStatus::MalformedFrame;
            result.Error = "Terminal peer did not return a Hello frame";
            return result;
        }

        const auto decoded = DecodeHelloResponse(_text(received.Frame.Payload));
        if (!decoded.Succeeded())
        {
            result.Status = RuntimeTransportStatus::MalformedFrame;
            result.Error = decoded.Error;
            return result;
        }
        const auto decision = ValidateHelloResponse(request, decoded.Value);
        result.HandshakeError = decision.Error;
        if (!decision.Accepted ||
            decision.Response.RuntimeIdentity != endpoint->RuntimeIdentity ||
            decision.Response.RuntimeInstanceId != expectedRuntimeInstanceId ||
            !HasCapability(
                decision.Response.Capabilities,
                ProtocolCapability::TerminalStream))
        {
            result.Status = RuntimeTransportStatus::ProtocolRejected;
            result.Error = decision.Message.empty() ?
                               "Terminal Hello response identity or capability is invalid" :
                               decision.Message;
            return result;
        }
        if (decision.Response.MaximumTerminalFrameBytes !=
            channel.MaximumPayloadBytes())
        {
            result.Status = RuntimeTransportStatus::ProtocolRejected;
            result.Error = "Terminal payload negotiation differs from the channel bound";
            return result;
        }

        result.Status = RuntimeTransportStatus::Succeeded;
        result.HandshakeError = HandshakeError::None;
        result.Response = decision.Response;
        return result;
    }

    RuntimeTerminalServerHandshakeResult PerformRuntimeTerminalServerHandshake(
        RuntimeTerminalChannel& channel,
        const RuntimeHandshakeConfiguration& runtime,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        RuntimeTerminalServerHandshakeResult result;
        const auto endpoint = channel.Endpoint();
        if (!channel || !endpoint ||
            runtime.RuntimeIdentity != endpoint->RuntimeIdentity ||
            runtime.MaximumTerminalFrameBytes != channel.MaximumPayloadBytes() ||
            !HasCapability(runtime.Capabilities, ProtocolCapability::TerminalStream))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error =
                "Terminal runtime configuration does not match its channel";
            return result;
        }
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        if (!_requireRemaining(deadline, result))
        {
            return result;
        }
        const auto received = channel.Receive(
            _remaining(deadline),
            cancellationEvent);
        if (!received.Succeeded())
        {
            static_cast<RuntimeTransportResult&>(result) = received;
            return result;
        }
        if (received.Frame.Kind != TerminalFrameKind::Hello ||
            received.Frame.Sequence != 1)
        {
            result.Status = RuntimeTransportStatus::MalformedFrame;
            result.Error = "Terminal connection did not begin with Hello sequence 1";
            _sendRejection(channel, deadline, cancellationEvent);
            return result;
        }

        const auto decoded = DecodeHelloRequest(_text(received.Frame.Payload));
        if (!decoded.Succeeded())
        {
            result.Status = RuntimeTransportStatus::MalformedFrame;
            result.Error = decoded.Error;
            _sendRejection(channel, deadline, cancellationEvent);
            return result;
        }
        result.Request = decoded.Value;
        if (result.Request.ExpectedRuntimeIdentity != endpoint->RuntimeIdentity ||
            !_requiresTerminalStream(result.Request))
        {
            result.Status = RuntimeTransportStatus::ProtocolRejected;
            result.Error =
                "Terminal Hello identity or required capability is invalid";
            _sendRejection(channel, deadline, cancellationEvent);
            return result;
        }

        result.Decision = NegotiateHandshake(result.Request, runtime);
        if (!result.Decision.Accepted)
        {
            result.Status = RuntimeTransportStatus::ProtocolRejected;
            result.Error = result.Decision.Message;
            _sendRejection(channel, deadline, cancellationEvent);
            return result;
        }
        const auto encoded = EncodeHelloResponse(result.Decision.Response);
        if (!encoded.Succeeded)
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = encoded.Error;
            return result;
        }

        TerminalFrame response;
        response.Kind = TerminalFrameKind::Hello;
        response.Sequence = 1;
        response.Generation = 0;
        response.Payload = _payload(encoded.Payload);
        if (!_requireRemaining(deadline, result))
        {
            return result;
        }
        const auto sent = channel.Send(
            response,
            _remaining(deadline),
            cancellationEvent);
        if (!sent.Succeeded())
        {
            static_cast<RuntimeTransportResult&>(result) = sent;
            return result;
        }
        result.Status = RuntimeTransportStatus::Succeeded;
        return result;
    }
}
