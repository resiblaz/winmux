// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "CodexAppServerContract.h"
#include "CodexAppServerEventParser.h"
#include "TaskProviderFailurePolicy.h"
#include "TaskProviderRunContext.h"
#include "TaskProviderVtRenderer.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskProvider;

namespace TaskProviderLocalTests
{
    class TaskProviderPolicyTests
    {
        TEST_CLASS(TaskProviderPolicyTests);

        TEST_METHOD(ValidatesPinnedMethodContract);
        TEST_METHOD(ParsesCurrentInitializeShape);
        TEST_METHOD(ClassifiesRetryAndResumeBoundaries);
        TEST_METHOD(PreparesScopedRtaskContextWithoutOverrides);
        TEST_METHOD(NormalizesRedactsAndBoundsEvents);
        TEST_METHOD(NormalizesSupportedNotificationAndApprovalMatrix);
        TEST_METHOD(RendersStructuredEventsAsBoundedVt);
    };

    void TaskProviderPolicyTests::ValidatesPinnedMethodContract()
    {
        constexpr std::string_view valid = R"({"methods":["thread/start","thread/resume","turn/start","turn/steer","turn/interrupt","thread/unsubscribe","item/commandExecution/requestApproval","item/fileChange/requestApproval","item/permissions/requestApproval","item/started","turn/completed"]})";
        const auto accepted = ValidateCodexAppServerSchema(valid);
        VERIFY_IS_TRUE(accepted.Succeeded());
        VERIFY_IS_TRUE(accepted.Value.has_value());
        VERIFY_ARE_EQUAL(RequiredTaskProviderCapabilities, accepted.Value->Capabilities);

        constexpr std::string_view missing = R"({"methods":["thread/start"]})";
        const auto rejected = ValidateCodexAppServerSchema(missing);
        VERIFY_IS_TRUE(rejected.Status == TaskProviderStatus::UnsupportedCapability);
        VERIFY_IS_FALSE(ValidateCodexAppServerSchema("{}").Succeeded());
    }

    void TaskProviderPolicyTests::ParsesCurrentInitializeShape()
    {
        Json::Value initialize{ Json::objectValue };
        initialize["userAgent"] = "codex_cli_rs/0.151.0";
        initialize["codexHome"] = "C:/fake/codex";
        const auto parsed = ParseCodexAppServerCapabilities(initialize);
        VERIFY_IS_TRUE(parsed.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "codex" }, parsed.Value->Provider);
        VERIFY_ARE_EQUAL(uint32_t{ 2 }, parsed.Value->ProtocolMajor);

        initialize.removeMember("userAgent");
        VERIFY_IS_TRUE(ParseCodexAppServerCapabilities(initialize).Status == TaskProviderStatus::UnsupportedVersion);
        initialize["userAgent"] = "richer-terminal-task-runtime/0.149.0";
        VERIFY_IS_TRUE(ParseCodexAppServerCapabilities(initialize).Status == TaskProviderStatus::UnsupportedVersion);
        initialize["userAgent"] = "richer-terminal-task-runtime/0.151.0 (Windows; x86_64)";
        VERIFY_IS_TRUE(ParseCodexAppServerCapabilities(initialize).Succeeded());
    }

    void TaskProviderPolicyTests::ClassifiesRetryAndResumeBoundaries()
    {
        auto disposition = ClassifyTaskProviderFailure(TaskProviderStatus::TransportClosed, "");
        VERIFY_IS_TRUE(disposition.Classification == TaskProviderFailureClass::RuntimeRecovery);
        VERIFY_IS_TRUE(disposition.ResumeSafety == TaskProviderResumeSafety::Safe);
        VERIFY_IS_TRUE(disposition.AutomaticRetryAllowed);
        VERIFY_ARE_EQUAL(uint32_t{ 2 }, disposition.MaximumExecutions);

        disposition = ClassifyTaskProviderFailure(TaskProviderStatus::Rejected, "network");
        VERIFY_IS_TRUE(disposition.Classification == TaskProviderFailureClass::TransientNetwork);
        VERIFY_ARE_EQUAL(uint32_t{ 3 }, disposition.MaximumExecutions);

        disposition = ClassifyTaskProviderFailure(TaskProviderStatus::Rejected, "context_overflow");
        VERIFY_IS_TRUE(disposition.ResumeSafety == TaskProviderResumeSafety::FreshSessionRequired);
        VERIFY_IS_FALSE(disposition.AutomaticRetryAllowed);

        disposition = ClassifyTaskProviderFailure(TaskProviderStatus::Rejected, "quota");
        VERIFY_IS_FALSE(disposition.AutomaticRetryAllowed);
    }

    void TaskProviderPolicyTests::PreparesScopedRtaskContextWithoutOverrides()
    {
        TaskProviderRunContext context;
        context.TaskId = "10000000-0000-4000-8000-000000000001";
        context.RunId = "20000000-0000-4000-8000-000000000002";
        context.WorkingDirectory = LR"(C:\work\task)";
        context.RtaskExecutable = LR"(C:\bin\rtask.exe)";
        context.RtaskEndpoint = R"(\\.\pipe\task-runtime)";
        context.RtaskCapability = "capability-value";
        context.Instructions = "Implement the task.";
        context.Environment.push_back({ L"SAFE_VALUE", L"one" });

        auto prepared = PrepareTaskProviderRunContext(context);
        VERIFY_IS_TRUE(prepared.Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 5 }, prepared.Environment.size());
        VERIFY_IS_TRUE(prepared.Instructions.find("[Richer Task Runtime]") != std::string::npos);

        context.Environment.push_back({ L"RICHER_TASK_CAPABILITY", L"override" });
        prepared = PrepareTaskProviderRunContext(context);
        VERIFY_IS_FALSE(prepared.Succeeded);
        VERIFY_IS_TRUE(prepared.Environment.empty());
    }

    void TaskProviderPolicyTests::NormalizesRedactsAndBoundsEvents()
    {
        Json::Value message{ Json::objectValue };
        message["method"] = "item/agentMessage/delta";
        message["params"]["threadId"] = "thread-1";
        message["params"]["turnId"] = "turn-1";
        message["params"]["itemId"] = "item-1";
        message["params"]["delta"] = "hello";
        message["params"]["accessToken"] = "must-not-survive";
        const auto parsed = ParseCodexAppServerEvent(message, 1);
        VERIFY_IS_TRUE(parsed.Succeeded());
        VERIFY_IS_TRUE(parsed.Event.has_value());
        VERIFY_IS_TRUE(parsed.Event->Kind == TaskProviderEventKind::AgentMessageDelta);
        VERIFY_ARE_EQUAL(std::string{ "hello" }, parsed.Event->Text);
        VERIFY_IS_TRUE(parsed.Event->Redacted);
        VERIFY_IS_TRUE(parsed.Event->PayloadJson.find("must-not-survive") == std::string::npos);

        message["method"] = "future/optional";
        const auto unknown = ParseCodexAppServerEvent(message, 2);
        VERIFY_IS_TRUE(unknown.Succeeded());
        VERIFY_IS_TRUE(unknown.Event->Kind == TaskProviderEventKind::UnknownOptional);

        message["method"] = "item/commandExecution/requestApproval";
        message["id"] = Json::UInt64{ 42 };
        message["params"]["approvalId"] = "approval-1";
        message["params"]["reason"] = "Run tests";
        const auto approval = ParseCodexAppServerEvent(message, 3);
        VERIFY_IS_TRUE(approval.Succeeded());
        VERIFY_IS_TRUE(approval.Approval.has_value());
        VERIFY_ARE_EQUAL(std::string{ "approval-1" }, approval.Event->ApprovalId);
        VERIFY_ARE_EQUAL(std::string{ "accept" }, CodexApprovalResponse(*approval.Approval, TaskProviderApprovalDecision::Approved)["decision"].asString());

        message.removeMember("params");
        VERIFY_IS_TRUE(ParseCodexAppServerEvent(message, 4).Status == TaskProviderStatus::MalformedResponse);
    }

    void TaskProviderPolicyTests::RendersStructuredEventsAsBoundedVt()
    {
        TaskProviderEvent event;
        event.Kind = TaskProviderEventKind::ToolStarted;
        event.Text = "build";
        auto rendered = RenderTaskProviderEventAsVt(event);
        VERIFY_IS_TRUE(rendered.Succeeded);
        VERIFY_IS_TRUE(rendered.Bytes.find("[tool] build") != std::string::npos);

        event.Kind = TaskProviderEventKind::RunFailed;
        event.Text = "network";
        rendered = RenderTaskProviderEventAsVt(event);
        VERIFY_IS_TRUE(rendered.Bytes.find("[error]") != std::string::npos);

        event.Text.assign(MaximumProviderTextBytes + 1, 'x');
        VERIFY_IS_FALSE(RenderTaskProviderEventAsVt(event).Succeeded);
    }

    void TaskProviderPolicyTests::NormalizesSupportedNotificationAndApprovalMatrix()
    {
        const auto message = [](const std::string& method) {
            Json::Value value{ Json::objectValue };
            value["method"] = method;
            value["params"]["threadId"] = "thread-1";
            value["params"]["turnId"] = "turn-1";
            value["params"]["itemId"] = "item-1";
            return value;
        };
        uint64_t sequence{ 1 };
        auto thread = message("thread/started");
        thread["params"].removeMember("threadId");
        thread["params"]["thread"]["id"] = "thread-1";
        auto parsed = ParseCodexAppServerEvent(thread, sequence++);
        VERIFY_IS_TRUE(parsed.Succeeded());
        VERIFY_IS_TRUE(parsed.Event->Kind == TaskProviderEventKind::SessionStarted);
        VERIFY_ARE_EQUAL(std::string{ "thread-1" }, parsed.Event->SessionId);

        auto turn = message("turn/started");
        turn["params"]["turn"]["id"] = "turn-1";
        parsed = ParseCodexAppServerEvent(turn, sequence++);
        VERIFY_IS_TRUE(parsed.Event->Kind == TaskProviderEventKind::RunStarted);

        const std::array itemCases{
            std::pair{ "commandExecution", TaskProviderEventKind::ToolStarted },
            std::pair{ "mcpToolCall", TaskProviderEventKind::ToolStarted },
            std::pair{ "fileChange", TaskProviderEventKind::FileChanged },
        };
        for (const auto& [type, expected] : itemCases)
        {
            auto item = message("item/started");
            item["params"]["item"]["id"] = "item-1";
            item["params"]["item"]["type"] = type;
            item["params"]["item"]["name"] = type;
            parsed = ParseCodexAppServerEvent(item, sequence++);
            VERIFY_IS_TRUE(parsed.Succeeded());
            VERIFY_IS_TRUE(parsed.Event->Kind == expected);
            item["method"] = "item/completed";
            parsed = ParseCodexAppServerEvent(item, sequence++);
            VERIFY_IS_TRUE(parsed.Succeeded());
            VERIFY_IS_TRUE(parsed.Event->Kind == (type == std::string_view{ "fileChange" } ? TaskProviderEventKind::FileChanged : TaskProviderEventKind::ToolCompleted));
        }

        const std::array notificationCases{
            std::pair{ "item/commandExecution/outputDelta", TaskProviderEventKind::ToolProgress },
            std::pair{ "item/mcpToolCall/progress", TaskProviderEventKind::ToolProgress },
            std::pair{ "item/fileChange/outputDelta", TaskProviderEventKind::FileChanged },
            std::pair{ "turn/plan/updated", TaskProviderEventKind::PlanUpdated },
            std::pair{ "turn/diff/updated", TaskProviderEventKind::DiffUpdated },
            std::pair{ "thread/tokenUsage/updated", TaskProviderEventKind::TokenUsage },
            std::pair{ "error", TaskProviderEventKind::ProviderError },
        };
        for (const auto& [method, expected] : notificationCases)
        {
            auto notification = message(method);
            notification["params"]["delta"] = "progress";
            notification["params"]["message"] = "progress";
            notification["params"]["error"]["message"] = "failure";
            parsed = ParseCodexAppServerEvent(notification, sequence++);
            VERIFY_IS_TRUE(parsed.Succeeded());
            VERIFY_IS_TRUE(parsed.Event->Kind == expected);
        }

        for (const auto& [status, expected] : std::array{
                 std::pair{ "completed", TaskProviderEventKind::RunCompleted },
                 std::pair{ "interrupted", TaskProviderEventKind::RunInterrupted },
                 std::pair{ "failed", TaskProviderEventKind::RunFailed },
             })
        {
            auto completed = message("turn/completed");
            completed["params"]["turn"]["id"] = "turn-1";
            completed["params"]["turn"]["status"] = status;
            completed["params"]["turn"]["error"]["message"] = "terminal";
            parsed = ParseCodexAppServerEvent(completed, sequence++);
            VERIFY_IS_TRUE(parsed.Succeeded());
            VERIFY_IS_TRUE(parsed.Event->Kind == expected);
        }

        for (const auto method : {
                 "item/commandExecution/requestApproval",
                 "item/fileChange/requestApproval",
                 "item/permissions/requestApproval",
             })
        {
            auto approval = message(method);
            approval["id"] = Json::UInt64{ sequence };
            approval["params"]["approvalId"] = "approval-1";
            approval["params"]["permissions"] = Json::Value{ Json::objectValue };
            parsed = ParseCodexAppServerEvent(approval, sequence++);
            VERIFY_IS_TRUE(parsed.Succeeded());
            VERIFY_IS_TRUE(parsed.Approval.has_value());
            VERIFY_IS_TRUE(parsed.Event->Kind == TaskProviderEventKind::ApprovalRequested);
            const auto denied = CodexApprovalResponse(*parsed.Approval, TaskProviderApprovalDecision::Denied);
            VERIFY_IS_TRUE(denied.isObject());
        }

        auto delta = message("item/agentMessage/delta");
        delta["params"]["delta"] = std::string(4096, 'x');
        parsed = ParseCodexAppServerEvent(delta, sequence++);
        VERIFY_ARE_EQUAL(size_t{ 4096 }, parsed.Event->Text.size());
    }
}
