// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "ProjectCatalogRpc.h"
#include "TaskBoardRpc.h"
#include "TaskSessionIndexRpc.h"
#include "ClientRequestPolicy.h"
#include "ProtocolHandshake.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        const std::string commandId{ "10000000-0000-4000-8000-000000000001" };
        const std::string projectId{ "20000000-0000-4000-8000-000000000002" };
        const std::string taskId{ "30000000-0000-4000-8000-000000000003" };
    }
    class ProjectBoardRpcTests
    {
        TEST_CLASS(ProjectBoardRpcTests);
        TEST_METHOD(ProjectRequestsRejectUntrustedIdentityAndUnknownFields);
        TEST_METHOD(BoardCommandsKeepIdentityAndAcceptanceStrict);
        TEST_METHOD(SummaryPagesRejectTruncationDuplicatesAndInvalidCursors);
        TEST_METHOD(ProjectBoardCapabilitiesRequireMinorFourAndOptIn);
        TEST_METHOD(NewQueriesRetryAndMutationsRetainUnknownOutcome);
        TEST_METHOD(SessionIndexUsesABoundedMonotonicCursor);
    };
    void ProjectBoardRpcTests::ProjectRequestsRejectUntrustedIdentityAndUnknownFields()
    {
        ProjectCatalogCommandRequest request{ commandId, ProjectCatalogOperation::Create, projectId, 0, "Project", "Background", "C:/project" };
        const auto encoded = EncodeProjectCatalogCommandRequest(request); VERIFY_IS_TRUE(encoded.Succeeded);
        const auto decoded = DecodeProjectCatalogCommandRequest(encoded.Payload); VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(request.RootPath, decoded.Value.RootPath);
        auto unknown = encoded.Payload; unknown.insert(1, "\"rootIdentity\":\"untrusted\",");
        VERIFY_IS_FALSE(DecodeProjectCatalogCommandRequest(unknown).Succeeded());
        auto duplicate = encoded.Payload; duplicate.insert(1, "\"name\":\"replacement\",");
        VERIFY_IS_FALSE(DecodeProjectCatalogCommandRequest(duplicate).Succeeded());
        request.ExpectedVersion = 1; VERIFY_IS_FALSE(EncodeProjectCatalogCommandRequest(request).Succeeded);
        request.Operation = ProjectCatalogOperation::Archive; request.RootPath.clear(); VERIFY_IS_TRUE(EncodeProjectCatalogCommandRequest(request).Succeeded);
        request.Description.assign(16385, 'x'); VERIFY_IS_FALSE(EncodeProjectCatalogCommandRequest(request).Succeeded);
        ProjectCatalogQueryRequest query; query.Limit = 101; VERIFY_IS_FALSE(EncodeProjectCatalogQueryRequest(query).Succeeded);
        query.Limit = 100; query.ProjectId = projectId; query.AfterId = taskId; VERIFY_IS_FALSE(EncodeProjectCatalogQueryRequest(query).Succeeded);
    }
    void ProjectBoardRpcTests::BoardCommandsKeepIdentityAndAcceptanceStrict()
    {
        TaskBoardCommandRequest request;
        request.CommandId = commandId; request.ProjectId = projectId; request.TaskId = taskId; request.ExpectedTaskVersion = 4;
        request.Operation = TaskBoardOperation::UpdateAcceptance; request.Acceptance = { { 0, true }, { 1, false } };
        auto encoded = EncodeTaskBoardCommandRequest(request); VERIFY_IS_TRUE(encoded.Succeeded);
        auto decoded = DecodeTaskBoardCommandRequest(encoded.Payload); VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(commandId, decoded.Value.CommandId); VERIFY_ARE_EQUAL(size_t{ 2 }, decoded.Value.Acceptance.size());
        request.Acceptance.push_back({ 0, false }); VERIFY_IS_FALSE(EncodeTaskBoardCommandRequest(request).Succeeded);
        request.Acceptance.clear(); request.Operation = TaskBoardOperation::Reorder; request.AnchorId = taskId;
        request.Placement = TaskBoardPlacement::Before; VERIFY_IS_FALSE(EncodeTaskBoardCommandRequest(request).Succeeded);
        request.AnchorId = commandId; VERIFY_IS_TRUE(EncodeTaskBoardCommandRequest(request).Succeeded);
        request.Text = std::string{ "bad\0text", 8 }; VERIFY_IS_FALSE(EncodeTaskBoardCommandRequest(request).Succeeded);
        request.Text.clear(); request.AnchorId.reset(); request.Placement = TaskBoardPlacement::End; request.Operation = TaskBoardOperation::Undo;
        VERIFY_IS_FALSE(EncodeTaskBoardCommandRequest(request).Succeeded);
        request.UndoOfCommandId = projectId; VERIFY_IS_TRUE(EncodeTaskBoardCommandRequest(request).Succeeded);
        TaskBoardCommandResult started{ commandId, taskId, 5, 2, 6, projectId, std::nullopt, true, false };
        VERIFY_IS_TRUE(DecodeTaskBoardCommandResult(EncodeTaskBoardCommandResult(started).Payload).Succeeded());
        started.RunId.reset(); VERIFY_IS_TRUE(EncodeTaskBoardCommandResult(started).Succeeded);
        started.TaskVersion = 0; VERIFY_IS_FALSE(EncodeTaskBoardCommandResult(started).Succeeded);
        VERIFY_IS_TRUE(DecodeTaskBoardOperationQueryRequest(EncodeTaskBoardOperationQueryRequest({ commandId }).Payload).Succeeded());
    }
    void ProjectBoardRpcTests::SummaryPagesRejectTruncationDuplicatesAndInvalidCursors()
    {
        TaskSummaryQueryRequest query; query.Sort = TaskSummaryOrder::BoardOrder;
        VERIFY_IS_FALSE(EncodeTaskSummaryQueryRequest(query).Succeeded);
        query.ProjectId = projectId; query.Bucket = TaskProjectionStatus::Todo; query.ExpectedColumnRevision = 3;
        VERIFY_IS_TRUE(DecodeTaskSummaryQueryRequest(EncodeTaskSummaryQueryRequest(query).Payload).Succeeded());
        TaskSummaryQueryResult page; page.Total = 5000; page.Counts[1] = 5000; page.ColumnRevision = 3;
        TaskSummaryProjection item; item.Id = taskId; item.ProjectId = projectId; item.ExecutionContextId = commandId;
        item.Title = "Task"; item.Version = 1; item.ColumnRevision = 3;
        page.Tasks.push_back(item); page.NextAfterId = taskId; page.NextAfterValue = 1024;
        auto encoded = EncodeTaskSummaryQueryResult(page); VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_ARE_EQUAL(uint64_t{ 5000 }, DecodeTaskSummaryQueryResult(encoded.Payload).Value.Total);
        page.Tasks.push_back(item); VERIFY_IS_FALSE(EncodeTaskSummaryQueryResult(page).Succeeded); page.Tasks.pop_back();
        ++page.Total; VERIFY_IS_FALSE(EncodeTaskSummaryQueryResult(page).Succeeded); --page.Total;
        page.NextAfterId = commandId; VERIFY_IS_FALSE(EncodeTaskSummaryQueryResult(page).Succeeded);
        VERIFY_IS_FALSE(DecodeTaskSummaryQueryResult(encoded.Payload.substr(0, encoded.Payload.size() - 1)).Succeeded());
    }
    void ProjectBoardRpcTests::ProjectBoardCapabilitiesRequireMinorFourAndOptIn()
    {
        HelloRequest request; request.ClientInstanceId = commandId; request.ExpectedRuntimeIdentity = "wt-task-runtime-v1:" + std::string(64, 'a');
        RuntimeHandshakeConfiguration runtime; runtime.RuntimeIdentity = request.ExpectedRuntimeIdentity; runtime.RuntimeInstanceId = projectId;
        runtime.Capabilities = AllProtocolCapabilities;
        for (const auto capability : { ProtocolCapability::ProjectCatalog, ProtocolCapability::TaskBoard })
        {
            request.Versions.PreferredMinor = 4; request.OptionalCapabilities = 0; request.RequiredCapabilities = 0;
            VERIFY_IS_FALSE(HasCapability(NegotiateHandshake(request, runtime).Response.Capabilities, capability));
            request.OptionalCapabilities = static_cast<uint64_t>(capability);
            VERIFY_IS_TRUE(HasCapability(NegotiateHandshake(request, runtime).Response.Capabilities, capability));
            request.Versions.PreferredMinor = 3;
            VERIFY_IS_FALSE(HasCapability(NegotiateHandshake(request, runtime).Response.Capabilities, capability));
            request.RequiredCapabilities = request.OptionalCapabilities; request.OptionalCapabilities = 0;
            VERIFY_IS_TRUE(NegotiateHandshake(request, runtime).Error == HandshakeError::MissingRequiredCapability);
        }
    }
    void ProjectBoardRpcTests::SessionIndexUsesABoundedMonotonicCursor()
    {
        VERIFY_IS_TRUE(EncodeTaskSessionIndexQueryRequest({ 10, 100 }).Succeeded);
        VERIFY_IS_FALSE(EncodeTaskSessionIndexQueryRequest({ 10, 101 }).Succeeded);
        TaskSessionIndexQueryResult page{ { { 11, "owned-session" } }, 11, false };
        const auto encoded = EncodeTaskSessionIndexQueryResult(page); VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_IS_TRUE(DecodeTaskSessionIndexQueryResult(encoded.Payload).Succeeded());
        page.Sessions.push_back({ 11, "duplicate-sequence" }); VERIFY_IS_FALSE(EncodeTaskSessionIndexQueryResult(page).Succeeded);
        VERIFY_IS_TRUE(IsReadOnlyControlMethod(ControlMethod::QueryTaskSessionIndex));
        VERIFY_IS_TRUE(EvaluateClientDisconnect(ControlMethod::QueryTaskSessionIndex, ClientRequestTransmission::WriteCompleted) == ClientDisconnectAction::RetryWithNewRequestId);
    }

    void ProjectBoardRpcTests::NewQueriesRetryAndMutationsRetainUnknownOutcome()
    {
        for (const auto method : { ControlMethod::QueryProjects, ControlMethod::QueryTaskSummaries, ControlMethod::QueryBoardOperation })
        {
            VERIFY_IS_TRUE(IsReadOnlyControlMethod(method));
            VERIFY_IS_TRUE(EvaluateClientDisconnect(method, ClientRequestTransmission::WriteCompleted) == ClientDisconnectAction::RetryWithNewRequestId);
            VERIFY_IS_TRUE(ControlMethodFromName(ControlMethodName(method)) == method);
        }
        for (const auto method : { ControlMethod::ProjectCommand, ControlMethod::TaskBoardCommand })
        {
            VERIFY_IS_FALSE(IsReadOnlyControlMethod(method));
            VERIFY_IS_TRUE(EvaluateClientDisconnect(method, ClientRequestTransmission::WriteStarted) == ClientDisconnectAction::OutcomeUnknown);
            VERIFY_IS_TRUE(EvaluateClientDisconnect(method, ClientRequestTransmission::NotSent) == ClientDisconnectAction::RetryWithNewRequestId);
        }
    }
}
