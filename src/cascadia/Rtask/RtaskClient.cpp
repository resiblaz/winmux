// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RtaskClient.h"

#include "RuntimeCrypto.h"

#include "ProtocolCapabilities.h"
#include "ProtocolRpcJson.h"
#include "ProtocolValidation.h"
#include "RtaskCommandJson.h"

namespace Microsoft::Terminal::Rtask
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        bool _validTimeout(const uint32_t value) noexcept
        {
            return value != 0 && value <= MaximumRuntimeTransportTimeoutMilliseconds;
        }

        RtaskClientResult _failure(
            const RtaskClientStatus status,
            std::string error)
        {
            RtaskClientResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }
    }

    std::optional<std::string> SystemRtaskIdSource::Next()
    {
        return NewRuntimeUuid();
    }

    NamedPipeRtaskClient::NamedPipeRtaskClient(
        RtaskClientConfiguration configuration,
        std::shared_ptr<IRuntimeControlConnector> connector,
        std::shared_ptr<IRtaskIdSource> ids) :
        _configuration{ std::move(configuration) },
        _connector{ connector ?
                        std::move(connector) :
                        std::make_shared<RuntimeNamedPipeControlConnector>() },
        _ids{ ids ? std::move(ids) : std::make_shared<SystemRtaskIdSource>() }
    {
        _valid = _connector && _ids &&
                 _validTimeout(_configuration.ConnectTimeoutMilliseconds) &&
                 _validTimeout(_configuration.SendTimeoutMilliseconds) &&
                 _validTimeout(_configuration.ReceiveTimeoutMilliseconds);
    }

    bool NamedPipeRtaskClient::Valid() const noexcept
    {
        return _valid;
    }

    RtaskClientResult NamedPipeRtaskClient::Execute(
        const RtaskContext& context,
        const RtaskCommandRequest& request)
    {
        if (!_valid)
        {
            return _failure(
                RtaskClientStatus::InvalidConfiguration,
                "rtask client is invalid");
        }
        try
        {
            std::string validationError;
            if (context.Identity.ControlPipeName.empty() ||
                !IsValidRuntimeIdentity(context.Identity.ProtocolIdentity) ||
                context.RunId != request.RunId ||
                context.Capability != request.Capability ||
                !ValidateRtaskCommand(request, validationError))
            {
                return _failure(
                    RtaskClientStatus::InvalidRequest,
                    "rtask request does not match its injected context");
            }
            const auto clientId = _ids->Next();
            const auto requestId = _ids->Next();
            if (!clientId || !requestId ||
                !IsCanonicalProtocolUuid(*clientId) ||
                !IsCanonicalProtocolUuid(*requestId))
            {
                return _failure(
                    RtaskClientStatus::IdentityFailure,
                    "rtask could not create protocol correlation IDs");
            }

            HelloRequest hello;
            hello.ClientInstanceId = *clientId;
            hello.ClientKind = ProtocolClientKind::TaskCtl;
            hello.ExpectedRuntimeIdentity = context.Identity.ProtocolIdentity;
            hello.RequiredCapabilities = static_cast<uint64_t>(
                ProtocolCapability::TaskCommands);
            auto connected = _connector->Connect(
                context.Identity,
                hello,
                _configuration.ConnectTimeoutMilliseconds);
            if (!connected.Succeeded())
            {
                return _failure(
                    RtaskClientStatus::ConnectFailure,
                    connected.Error.empty() ?
                        "rtask could not connect to Task Runtime" :
                        connected.Error);
            }

            const auto params = EncodeRtaskCommandRequest(request);
            if (!params.Succeeded)
            {
                connected.Connection->Close();
                return _failure(
                    RtaskClientStatus::InvalidRequest,
                    "rtask command encoding failed");
            }
            ControlRequest control;
            control.RequestId = *requestId;
            control.Method = ControlMethod::SubmitCommand;
            control.PayloadJson = params.Payload;
            const auto encoded = EncodeControlRequest(control);
            if (!encoded.Succeeded)
            {
                connected.Connection->Close();
                return _failure(
                    RtaskClientStatus::ProtocolFailure,
                    "rtask request envelope encoding failed");
            }
            const auto sent = connected.Connection->Send(
                encoded.Payload,
                _configuration.SendTimeoutMilliseconds);
            if (!sent.Succeeded())
            {
                connected.Connection->Close();
                return _failure(
                    RtaskClientStatus::SendFailure,
                    sent.Error.empty() ?
                        "rtask request send failed" :
                        sent.Error);
            }
            const auto received = connected.Connection->Receive(
                _configuration.ReceiveTimeoutMilliseconds);
            connected.Connection->Close();
            if (!received.Succeeded())
            {
                return _failure(
                    RtaskClientStatus::ReceiveFailure,
                    received.Error.empty() ?
                        "rtask response receive failed" :
                        received.Error);
            }
            const auto response = DecodeControlResponse(received.Payload);
            if (!response.Succeeded() || response.Value.RequestId != *requestId)
            {
                return _failure(
                    RtaskClientStatus::ProtocolFailure,
                    "rtask response envelope is invalid or uncorrelated");
            }
            if (!response.Value.Succeeded)
            {
                if (response.Value.Error)
                {
                    RtaskClientResult result;
                    result.Status = RtaskClientStatus::RpcError;
                    result.Error = response.Value.Error->Message;
                    result.RpcError = response.Value.Error;
                    return result;
                }
                return _failure(
                    RtaskClientStatus::ProtocolFailure,
                    "rtask error response is incomplete");
            }
            const auto commandResult = DecodeRtaskCommandResult(
                response.Value.ResultJson);
            if (!commandResult.Succeeded())
            {
                return _failure(
                    RtaskClientStatus::ProtocolFailure,
                    "rtask command result is invalid");
            }
            RtaskClientResult result;
            result.Status = RtaskClientStatus::Succeeded;
            result.Result = commandResult.Value;
            return result;
        }
        catch (...)
        {
            return _failure(
                RtaskClientStatus::ResourceFailure,
                "rtask client exhausted resources");
        }
    }
}
