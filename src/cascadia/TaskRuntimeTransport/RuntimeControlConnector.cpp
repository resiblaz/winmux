// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlConnector.h"

#include "RuntimeControlChannel.h"
#include "RuntimeControlHandshake.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        uint32_t _remaining(const uint64_t deadline) noexcept
        {
            const auto now = GetTickCount64();
            return now >= deadline ? 0 : static_cast<uint32_t>(deadline - now);
        }

        RuntimeControlConnectFailureReason _connectFailureReason(
            const RuntimeControlChannelResult& result) noexcept
        {
            if (result.Status == RuntimeTransportStatus::Cancelled)
            {
                return RuntimeControlConnectFailureReason::Cancelled;
            }
            if (result.Status == RuntimeTransportStatus::InvalidConfiguration)
            {
                return RuntimeControlConnectFailureReason::InvalidConfiguration;
            }
            if (result.Status == RuntimeTransportStatus::Timeout)
            {
                return result.EndpointObservation == RuntimeControlEndpointObservation::Busy ?
                           RuntimeControlConnectFailureReason::EndpointBusy :
                           RuntimeControlConnectFailureReason::DeadlineExpired;
            }
            if (result.EndpointObservation == RuntimeControlEndpointObservation::Absent &&
                result.Status == RuntimeTransportStatus::Disconnected &&
                result.SystemError == ERROR_FILE_NOT_FOUND)
            {
                return RuntimeControlConnectFailureReason::EndpointAbsent;
            }
            if (result.SystemError == ERROR_ACCESS_DENIED)
            {
                return RuntimeControlConnectFailureReason::EndpointAccessDenied;
            }
            if (result.Status == RuntimeTransportStatus::Disconnected ||
                result.Status == RuntimeTransportStatus::SystemError)
            {
                return RuntimeControlConnectFailureReason::EndpointTransportFailure;
            }
            return RuntimeControlConnectFailureReason::Unclassified;
        }

        RuntimeControlConnectFailureReason _handshakeFailureReason(
            const RuntimeTransportStatus status) noexcept
        {
            switch (status)
            {
            case RuntimeTransportStatus::Cancelled:
                return RuntimeControlConnectFailureReason::Cancelled;
            case RuntimeTransportStatus::Timeout:
                return RuntimeControlConnectFailureReason::HandshakeDeadlineExpired;
            case RuntimeTransportStatus::ProtocolRejected:
                return RuntimeControlConnectFailureReason::HandshakeRejected;
            case RuntimeTransportStatus::MalformedFrame:
                return RuntimeControlConnectFailureReason::HandshakeMalformed;
            case RuntimeTransportStatus::InvalidConfiguration:
                return RuntimeControlConnectFailureReason::InvalidConfiguration;
            case RuntimeTransportStatus::Disconnected:
            case RuntimeTransportStatus::SystemError:
                return RuntimeControlConnectFailureReason::HandshakeTransportFailure;
            default:
                return RuntimeControlConnectFailureReason::Unclassified;
            }
        }
    }

    RuntimeNamedPipeControlConnector::RuntimeNamedPipeControlConnector(
        const RuntimeControlEndpointDiscoveryPolicy discoveryPolicy) noexcept :
        _discoveryPolicy{ discoveryPolicy }
    {
    }

    RuntimeControlConnectionResult RuntimeNamedPipeControlConnector::Connect(
        const RuntimeIdentity& identity,
        const Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest& request,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent) noexcept
    {
        RuntimeControlConnectionResult result;
        try
        {
            if (timeoutMilliseconds == 0 ||
                timeoutMilliseconds > MaximumRuntimeTransportTimeoutMilliseconds)
            {
                result.Status = RuntimeTransportStatus::InvalidConfiguration;
                result.FailureReason = RuntimeControlConnectFailureReason::InvalidConfiguration;
                result.Error = "Runtime connector timeout is invalid";
                return result;
            }

            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            const auto connectBudget = _remaining(deadline);
            if (connectBudget == 0)
            {
                result.Status = RuntimeTransportStatus::Timeout;
                result.FailureReason = RuntimeControlConnectFailureReason::DeadlineExpired;
                result.Error = "Runtime connection deadline expired before pipe discovery";
                return result;
            }
            auto connected = ConnectRuntimeControlChannel(
                identity,
                connectBudget,
                cancellationEvent,
                _discoveryPolicy);
            static_cast<RuntimeTransportResult&>(result) =
                static_cast<RuntimeTransportResult&>(connected);
            if (!connected.Succeeded())
            {
                result.FailureReason = _connectFailureReason(connected);
                return result;
            }

            result.Phase = RuntimeControlConnectPhase::Handshaking;
            const auto remaining = _remaining(deadline);
            if (remaining == 0)
            {
                result.Status = RuntimeTransportStatus::Timeout;
                result.FailureReason = RuntimeControlConnectFailureReason::HandshakeDeadlineExpired;
                result.Error = "Runtime connection deadline expired before Hello";
                return result;
            }
            auto handshake = PerformRuntimeClientHandshake(
                connected.Channel,
                request,
                remaining,
                cancellationEvent);
            static_cast<RuntimeTransportResult&>(result) =
                static_cast<RuntimeTransportResult&>(handshake);
            result.HandshakeError = handshake.HandshakeError;
            result.Response = std::move(handshake.Response);
            if (!handshake.Succeeded())
            {
                result.FailureReason = _handshakeFailureReason(handshake.Status);
                return result;
            }

            result.Connection = std::make_unique<RuntimeControlChannel>(
                std::move(connected.Channel));
            result.Phase = RuntimeControlConnectPhase::Connected;
            result.Status = RuntimeTransportStatus::Succeeded;
            result.FailureReason = RuntimeControlConnectFailureReason::None;
            return result;
        }
        catch (const std::exception& exception)
        {
            result.Connection.reset();
            result.Status = RuntimeTransportStatus::SystemError;
            result.FailureReason = RuntimeControlConnectFailureReason::ResourceFailure;
            result.Error = exception.what();
            return result;
        }
        catch (...)
        {
            result.Connection.reset();
            result.Status = RuntimeTransportStatus::SystemError;
            result.FailureReason = RuntimeControlConnectFailureReason::ResourceFailure;
            result.Error = "Unknown Runtime connector failure";
            return result;
        }
    }
}
