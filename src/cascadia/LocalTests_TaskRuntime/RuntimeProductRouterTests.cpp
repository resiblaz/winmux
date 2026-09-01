// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeProductRouter.h"
#include "RuntimeRtaskCommandTestSupport.h"

#include "RuntimeFoundationRpc.h"
#include "RtaskCommandJson.h"
#include "TaskProjectionRpc.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::RuntimeRtaskCommandTestSupport;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr std::string_view RuntimeIdentity{
            "wt-task-runtime-v1:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
        };
        constexpr std::string_view RuntimeInstanceId{
            "ae000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view ClientId{
            "ae000000-0000-4000-8000-000000000002"
        };

        std::shared_ptr<RuntimeProductRouter> _router(Fixture& fixture)
        {
            return std::make_shared<RuntimeProductRouter>(
                std::make_shared<RuntimeFoundationRouter>(
                    std::string{ RuntimeIdentity },
                    std::string{ RuntimeInstanceId },
                    42),
                fixture.Service,
                nullptr,
                std::make_shared<RuntimeTaskProjectionService>(
                    fixture.Store,
                    nullptr,
                    LR"(C:\runtime-router-tests)"));
        }

        RuntimeControlCancellationToken _cancellation(const bool cancelled = false)
        {
            return RuntimeControlCancellationToken{
                std::make_shared<std::atomic_bool>(cancelled)
            };
        }

        RuntimeControlClientContext _client(const ProtocolClientKind kind)
        {
            return {
                std::string{ ClientId },
                kind,
                static_cast<uint64_t>(ProtocolCapability::TaskCommands) |
                    static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                    static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle),
            };
        }
    }

    class RuntimeProductRouterTests
    {
        TEST_CLASS(RuntimeProductRouterTests);

        TEST_METHOD(RoutesTaskCtlSubmitCommandToDurableService);
        TEST_METHOD(RejectsUiCancelledMalformedAndUnauthorizedSubmit);
        TEST_METHOD(PreservesFoundationPingAndRuntimeStatusRoutes);
        TEST_METHOD(RoutesTerminalUiCreateAndQueryToTaskProjectionService);
    };

    void RuntimeProductRouterTests::RoutesTaskCtlSubmitCommandToDurableService()
    {
        Fixture fixture;
        const auto issued = fixture.Issue(AllRtaskMethods, GetTickCount64());
        VERIFY_IS_TRUE(issued.Grant.has_value());
        if (!issued.Grant)
        {
            return;
        }
        const auto request = fixture.Request(
            *issued.Grant,
            RtaskMethod::StatusInProgress,
            1);
        ControlRequest control;
        control.RequestId = MakeId<Microsoft::Terminal::TaskCore::CommandId>(9000).Value();
        control.Method = ControlMethod::SubmitCommand;
        control.PayloadJson = EncodeRtaskCommandRequest(request).Payload;

        const auto router = _router(fixture);
        VERIFY_IS_TRUE(router->Valid());
        const auto routed = router->Route(
            _client(ProtocolClientKind::TaskCtl),
            control,
            _cancellation());
        VERIFY_IS_TRUE(routed.Succeeded);
        VERIFY_IS_FALSE(routed.Error.has_value());
        const auto result = DecodeRtaskCommandResult(routed.ResultJson);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(request.CommandId, result.Value.CommandId);
        VERIFY_IS_TRUE(result.Value.Disposition == RtaskCommandDisposition::Applied);
        const auto snapshot = fixture.Store->LoadTask(fixture.Task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Task.Status == Microsoft::Terminal::TaskCore::TaskStatus::InProgress);
    }

    void RuntimeProductRouterTests::RejectsUiCancelledMalformedAndUnauthorizedSubmit()
    {
        Fixture fixture;
        const auto issued = fixture.Issue(AllRtaskMethods, GetTickCount64());
        VERIFY_IS_TRUE(issued.Grant.has_value());
        if (!issued.Grant)
        {
            return;
        }
        auto request = fixture.Request(
            *issued.Grant,
            RtaskMethod::Result,
            1,
            "router result");
        ControlRequest control;
        control.RequestId = MakeId<Microsoft::Terminal::TaskCore::CommandId>(9001).Value();
        control.Method = ControlMethod::SubmitCommand;
        control.PayloadJson = EncodeRtaskCommandRequest(request).Payload;
        const auto router = _router(fixture);

        auto rejected = router->Route(
            _client(ProtocolClientKind::TerminalUi),
            control,
            _cancellation());
        VERIFY_IS_FALSE(rejected.Succeeded);
        VERIFY_IS_TRUE(rejected.Error.has_value());
        VERIFY_IS_TRUE(rejected.Error->Code == ProtocolRpcErrorCode::CapabilityRequired);

        rejected = router->Route(
            _client(ProtocolClientKind::TaskCtl),
            control,
            _cancellation(true));
        VERIFY_IS_TRUE(rejected.Error->Code == ProtocolRpcErrorCode::RuntimeUnavailable);

        control.PayloadJson = "{}";
        rejected = router->Route(
            _client(ProtocolClientKind::TaskCtl),
            control,
            _cancellation());
        VERIFY_IS_TRUE(rejected.Error->Code == ProtocolRpcErrorCode::InvalidRequest);

        request.Capability = std::string(RtaskCapabilityBytes, 'f');
        control.PayloadJson = EncodeRtaskCommandRequest(request).Payload;
        rejected = router->Route(
            _client(ProtocolClientKind::TaskCtl),
            control,
            _cancellation());
        VERIFY_IS_TRUE(rejected.Error->Code == ProtocolRpcErrorCode::CapabilityRequired);
        VERIFY_IS_TRUE(rejected.Error->Message.find(request.Capability) == std::string::npos);
        VERIFY_IS_TRUE(rejected.Error->Message.find(request.Text) == std::string::npos);
    }

    void RuntimeProductRouterTests::PreservesFoundationPingAndRuntimeStatusRoutes()
    {
        Fixture fixture;
        const auto router = _router(fixture);
        ControlRequest ping;
        ping.RequestId = MakeId<Microsoft::Terminal::TaskCore::CommandId>(9002).Value();
        ping.Method = ControlMethod::Ping;
        const auto pong = router->Route(
            _client(ProtocolClientKind::TerminalUi),
            ping,
            _cancellation());
        VERIFY_IS_TRUE(pong.Succeeded);
        const auto decodedPong = DecodePingResult(pong.ResultJson);
        VERIFY_IS_TRUE(decodedPong.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ RuntimeInstanceId }, decodedPong.Value.RuntimeInstanceId);

        ping.Method = ControlMethod::RuntimeStatus;
        const auto status = router->Route(
            _client(ProtocolClientKind::TerminalUi),
            ping,
            _cancellation());
        VERIFY_IS_TRUE(status.Succeeded);
        const auto decodedStatus = DecodeRuntimeStatusResult(status.ResultJson);
        VERIFY_IS_TRUE(decodedStatus.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ RuntimeIdentity }, decodedStatus.Value.RuntimeIdentity);
    }

    void RuntimeProductRouterTests::RoutesTerminalUiCreateAndQueryToTaskProjectionService()
    {
        Fixture fixture;
        const auto router = _router(fixture);
        TaskUiCommandRequest create;
        create.CommandId = MakeId<Microsoft::Terminal::TaskCore::CommandId>(9100).Value();
        create.TaskId = MakeId<Microsoft::Terminal::TaskCore::TaskId>(9101).Value();
        create.Operation = TaskUiCommandOperation::Create;
        create.Draft = TaskUiCreateDraft{};
        create.Draft->WorkspaceId = MakeId<Microsoft::Terminal::TaskCore::WorkspaceId>(9102).Value();
        create.Draft->Title = "Routed Task";
        create.Draft->SourceRoot = "C:/src/routed";
        create.Draft->ExecutionMode = TaskProjectionExecutionMode::Direct;
        ControlRequest control;
        control.RequestId = MakeId<Microsoft::Terminal::TaskCore::CommandId>(9103).Value();
        control.Method = ControlMethod::SubmitCommand;
        control.PayloadJson = EncodeTaskUiCommandRequest(create).Payload;
        auto routed = router->Route(_client(ProtocolClientKind::TerminalUi), control, _cancellation());
        VERIFY_IS_TRUE(routed.Succeeded);
        const auto created = DecodeTaskUiCommandResult(routed.ResultJson);
        VERIFY_IS_TRUE(created.Succeeded());
        VERIFY_ARE_EQUAL(create.TaskId, created.Value.TaskId);

        TaskProjectionQueryRequest query;
        query.TaskId = create.TaskId;
        query.IncludeCompleted = true;
        query.EventLimit = 16;
        control.Method = ControlMethod::QueryTask;
        control.PayloadJson = EncodeTaskProjectionQueryRequest(query).Payload;
        routed = router->Route(_client(ProtocolClientKind::TerminalUi), control, _cancellation());
        VERIFY_IS_TRUE(routed.Succeeded);
        const auto projected = DecodeTaskProjectionQueryResult(routed.ResultJson);
        VERIFY_IS_TRUE(projected.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, projected.Value.Tasks.size());
        VERIFY_ARE_EQUAL(std::string{ "Routed Task" }, projected.Value.Tasks.front().Title);

        routed = router->Route(_client(ProtocolClientKind::TaskCtl), control, _cancellation());
        VERIFY_IS_FALSE(routed.Succeeded);
        VERIFY_IS_TRUE(routed.Error->Code == ProtocolRpcErrorCode::CapabilityRequired);
    }
}
