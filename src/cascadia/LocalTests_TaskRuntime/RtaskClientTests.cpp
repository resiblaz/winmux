// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRtaskTransport.h"

#include "ProtocolCapabilities.h"
#include "ProtocolRpcJson.h"
#include "RtaskCommandJson.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::Rtask;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr std::string_view ClientId{
            "ad000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view RequestId{
            "ad000000-0000-4000-8000-000000000002"
        };
        constexpr std::string_view CommandId{
            "ad000000-0000-4000-8000-000000000003"
        };
        constexpr std::string_view RunId{
            "ad000000-0000-4000-8000-000000000004"
        };

        RtaskContext _context()
        {
            RtaskContext context;
            context.Identity.ControlPipeName =
                L"\\\\.\\pipe\\WT.TaskRuntime." + std::wstring(64, L'a') + L".control";
            context.Identity.ProtocolIdentity =
                "wt-task-runtime-v1:" + std::string(64, 'a');
            context.RunId = RunId;
            context.Capability = std::string(RtaskCapabilityBytes, 'b');
            return context;
        }

        RtaskCommandRequest _request()
        {
            return {
                std::string{ CommandId },
                std::string{ RunId },
                std::string(RtaskCapabilityBytes, 'b'),
                RtaskMethod::Result,
                "completed result",
            };
        }

        std::string _successResponse(const std::string_view requestId = RequestId)
        {
            const auto result = EncodeRtaskCommandResult({
                std::string{ CommandId },
                RtaskCommandDisposition::Applied,
                7,
                9,
            });
            ControlResponse response;
            response.RequestId = requestId;
            response.Succeeded = true;
            response.ResultJson = result.Payload;
            return EncodeControlResponse(response).Payload;
        }

        std::shared_ptr<FakeRtaskIdSource> _ids()
        {
            auto ids = std::make_shared<FakeRtaskIdSource>();
            ids->Ids.emplace_back(ClientId);
            ids->Ids.emplace_back(RequestId);
            return ids;
        }
    }

    class RtaskClientTests
    {
        TEST_CLASS(RtaskClientTests);

        TEST_METHOD(SendsHelloAndOneCorrelatedSubmitCommand);
        TEST_METHOD(ReturnsTypedRpcErrorWithoutLeakingRequestPayload);
        TEST_METHOD(ClassifiesConnectSendAndReceiveFailures);
        TEST_METHOD(RejectsIdentityContextAndCorrelationDrift);
    };

    void RtaskClientTests::SendsHelloAndOneCorrelatedSubmitCommand()
    {
        auto state = std::make_shared<FakeRtaskTransportState>();
        state->ReceiveResult.Status = RuntimeTransportStatus::Succeeded;
        state->ReceiveResult.Payload = _successResponse();
        auto connector = std::make_shared<FakeRtaskConnector>(state);
        NamedPipeRtaskClient client{ {}, connector, _ids() };
        VERIFY_IS_TRUE(client.Valid());

        const auto context = _context();
        const auto request = _request();
        const auto executed = client.Execute(context, request);
        VERIFY_IS_TRUE(executed.Status == RtaskClientStatus::Succeeded);
        VERIFY_ARE_EQUAL(std::string{ CommandId }, executed.Result.CommandId);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, executed.Result.TaskVersion);
        VERIFY_ARE_EQUAL(context.Identity.ControlPipeName, state->Identity.ControlPipeName);
        VERIFY_ARE_EQUAL(context.Identity.ProtocolIdentity, state->Hello.ExpectedRuntimeIdentity);
        VERIFY_ARE_EQUAL(std::string{ ClientId }, state->Hello.ClientInstanceId);
        VERIFY_IS_TRUE(state->Hello.ClientKind == ProtocolClientKind::TaskCtl);
        VERIFY_ARE_EQUAL(
            static_cast<uint64_t>(ProtocolCapability::TaskCommands),
            state->Hello.RequiredCapabilities);
        VERIFY_ARE_EQUAL(size_t{ 1 }, state->SentPayloads.size());
        VERIFY_IS_TRUE(state->Closed);

        const auto control = DecodeControlRequest(state->SentPayloads.front());
        VERIFY_IS_TRUE(control.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ RequestId }, control.Value.RequestId);
        VERIFY_IS_TRUE(control.Value.Method == ControlMethod::SubmitCommand);
        const auto command = DecodeRtaskCommandRequest(control.Value.PayloadJson);
        VERIFY_IS_TRUE(command.Succeeded());
        VERIFY_ARE_EQUAL(request.CommandId, command.Value.CommandId);
        VERIFY_ARE_EQUAL(request.Text, command.Value.Text);
    }

    void RtaskClientTests::ReturnsTypedRpcErrorWithoutLeakingRequestPayload()
    {
        auto state = std::make_shared<FakeRtaskTransportState>();
        ControlResponse response;
        response.RequestId = RequestId;
        response.Error = ProtocolRpcError{
            ProtocolRpcErrorCode::CapabilityRequired,
            "rtask capability authorization failed",
            R"({"reason":"authorization_failed"})",
        };
        state->ReceiveResult.Status = RuntimeTransportStatus::Succeeded;
        state->ReceiveResult.Payload = EncodeControlResponse(response).Payload;
        NamedPipeRtaskClient client{
            {},
            std::make_shared<FakeRtaskConnector>(state),
            _ids(),
        };
        const auto request = _request();
        const auto executed = client.Execute(_context(), request);
        VERIFY_IS_TRUE(executed.Status == RtaskClientStatus::RpcError);
        VERIFY_IS_TRUE(executed.RpcError.has_value());
        VERIFY_IS_TRUE(
            executed.RpcError->Code == ProtocolRpcErrorCode::CapabilityRequired);
        VERIFY_IS_TRUE(executed.Error.find(request.Capability) == std::string::npos);
        VERIFY_IS_TRUE(executed.Error.find(request.Text) == std::string::npos);
    }

    void RtaskClientTests::ClassifiesConnectSendAndReceiveFailures()
    {
        auto state = std::make_shared<FakeRtaskTransportState>();
        state->ConnectFails = true;
        NamedPipeRtaskClient connectFailure{
            {},
            std::make_shared<FakeRtaskConnector>(state),
            _ids(),
        };
        VERIFY_IS_TRUE(
            connectFailure.Execute(_context(), _request()).Status ==
            RtaskClientStatus::ConnectFailure);

        state = std::make_shared<FakeRtaskTransportState>();
        state->SendResult.Status = RuntimeTransportStatus::Timeout;
        NamedPipeRtaskClient sendFailure{
            {},
            std::make_shared<FakeRtaskConnector>(state),
            _ids(),
        };
        VERIFY_IS_TRUE(
            sendFailure.Execute(_context(), _request()).Status ==
            RtaskClientStatus::SendFailure);
        VERIFY_IS_TRUE(state->Closed);

        state = std::make_shared<FakeRtaskTransportState>();
        state->ReceiveResult.Status = RuntimeTransportStatus::Disconnected;
        NamedPipeRtaskClient receiveFailure{
            {},
            std::make_shared<FakeRtaskConnector>(state),
            _ids(),
        };
        VERIFY_IS_TRUE(
            receiveFailure.Execute(_context(), _request()).Status ==
            RtaskClientStatus::ReceiveFailure);
        VERIFY_IS_TRUE(state->Closed);
    }

    void RtaskClientTests::RejectsIdentityContextAndCorrelationDrift()
    {
        auto state = std::make_shared<FakeRtaskTransportState>();
        state->ReceiveResult.Status = RuntimeTransportStatus::Succeeded;
        state->ReceiveResult.Payload = _successResponse(
            "ad000000-0000-4000-8000-000000000099");
        NamedPipeRtaskClient uncorrelated{
            {},
            std::make_shared<FakeRtaskConnector>(state),
            _ids(),
        };
        VERIFY_IS_TRUE(
            uncorrelated.Execute(_context(), _request()).Status ==
            RtaskClientStatus::ProtocolFailure);

        auto request = _request();
        request.RunId = "ad000000-0000-4000-8000-000000000099";
        NamedPipeRtaskClient mismatched{
            {},
            std::make_shared<FakeRtaskConnector>(
                std::make_shared<FakeRtaskTransportState>()),
            _ids(),
        };
        VERIFY_IS_TRUE(
            mismatched.Execute(_context(), request).Status ==
            RtaskClientStatus::InvalidRequest);
        auto exhaustedIds = std::make_shared<FakeRtaskIdSource>();
        NamedPipeRtaskClient noIds{
            {},
            std::make_shared<FakeRtaskConnector>(
                std::make_shared<FakeRtaskTransportState>()),
            exhaustedIds,
        };
        VERIFY_IS_TRUE(
            noIds.Execute(_context(), _request()).Status ==
            RtaskClientStatus::IdentityFailure);
    }
}
