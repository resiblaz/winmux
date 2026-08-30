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
                result.Error = "Runtime connector timeout is invalid";
                return result;
            }

            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            const auto connectBudget = _remaining(deadline);
            if (connectBudget == 0)
            {
                result.Status = RuntimeTransportStatus::Timeout;
                result.Error = "Runtime connection deadline expired before pipe discovery";
                return result;
            }
            auto connected = ConnectRuntimeControlChannel(
                identity,
                connectBudget,
                cancellationEvent);
            static_cast<RuntimeTransportResult&>(result) =
                static_cast<RuntimeTransportResult&>(connected);
            if (!connected.Succeeded())
            {
                return result;
            }

            result.Phase = RuntimeControlConnectPhase::Handshaking;
            const auto remaining = _remaining(deadline);
            if (remaining == 0)
            {
                result.Status = RuntimeTransportStatus::Timeout;
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
                return result;
            }

            result.Connection = std::make_unique<RuntimeControlChannel>(
                std::move(connected.Channel));
            result.Phase = RuntimeControlConnectPhase::Connected;
            result.Status = RuntimeTransportStatus::Succeeded;
            return result;
        }
        catch (const std::exception& exception)
        {
            result.Connection.reset();
            result.Status = RuntimeTransportStatus::SystemError;
            result.Error = exception.what();
            return result;
        }
        catch (...)
        {
            result.Connection.reset();
            result.Status = RuntimeTransportStatus::SystemError;
            result.Error = "Unknown Runtime connector failure";
            return result;
        }
    }
}
