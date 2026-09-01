// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "CodexAppServerAdapter.h"
#include "CodexAppServerTransport.h"
#include "FakeCodexAppServerTransport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskProvider;

namespace TaskProviderLocalTests
{
    namespace
    {
        constexpr std::string_view TaskId{ "10000000-0000-4000-8000-000000000001" };
        constexpr std::string_view RunId{ "20000000-0000-4000-8000-000000000002" };

        TaskProviderRunRequest _runRequest()
        {
            TaskProviderRunRequest request;
            request.SessionId = "provider-thread-1";
            request.Prompt = "Implement the task";
            request.Context.TaskId = TaskId;
            request.Context.RunId = RunId;
            request.Context.WorkingDirectory = LR"(C:\work\task)";
            request.Context.Model = "gpt-5";
            request.Context.Instructions = "Keep changes scoped.";
            request.Context.RtaskExecutable = LR"(C:\bin\rtask.exe)";
            request.Context.RtaskEndpoint = R"(\\.\pipe\task-runtime)";
            request.Context.RtaskCapability = "run-capability";
            return request;
        }
    }

    class CodexAppServerAdapterTests
    {
        TEST_CLASS(CodexAppServerAdapterTests);

        TEST_METHOD(InitializesStartsResumesAndRunsWithScopedContext);
        TEST_METHOD(TranslatesEventsAndCorrelatesApprovalResponse);
        TEST_METHOD(PropagatesMethodErrorsAndTransportFailure);
        TEST_METHOD(YoloPrecedesAppServerSubcommandInProcessCommandLine);
        TEST_METHOD(OptInInstalledCodexInitializeSmoke);
    };

    void CodexAppServerAdapterTests::InitializesStartsResumesAndRunsWithScopedContext()
    {
        const auto state = std::make_shared<FakeCodexAppServerState>();
        CodexAppServerAdapter adapter{ MakeFakeCodexAppServerTransportFactory(state) };
        const auto capabilities = adapter.ProbeCapabilities(nullptr);
        VERIFY_IS_TRUE(capabilities.Succeeded());
        VERIFY_IS_TRUE(state->Started);

        TaskProviderSessionRequest sessionRequest;
        sessionRequest.TaskId = TaskId;
        sessionRequest.WorkingDirectory = LR"(C:\work\task)";
        sessionRequest.Model = "gpt-5";
        auto session = adapter.StartSession(sessionRequest, nullptr);
        VERIFY_IS_TRUE(session.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "provider-thread-1" }, session.Value->SessionId);

        session = adapter.ResumeSession("provider-thread-1", nullptr);
        VERIFY_IS_TRUE(session.Succeeded());

        const auto run = adapter.StartRun(_runRequest(), nullptr);
        VERIFY_IS_TRUE(run.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "provider-turn-1" }, run.Value->RunId);
        VERIFY_IS_TRUE(adapter.SteerRun("provider-thread-1", "provider-turn-1", "continue", nullptr).Succeeded());
        VERIFY_IS_TRUE(adapter.InterruptRun("provider-thread-1", "provider-turn-1", nullptr).Succeeded());
        VERIFY_IS_TRUE(adapter.SuspendSession("provider-thread-1", nullptr).Succeeded());
        VERIFY_IS_TRUE(adapter.CloseSessionProjection("provider-thread-1", nullptr).Succeeded());

        const auto writes = state->SnapshotWrites();
        const auto resume = std::find_if(writes.begin(), writes.end(), [](const auto& value) {
            return value.isMember("method") && value["method"] == "thread/resume" &&
                   value["params"].isMember("developerInstructions");
        });
        VERIFY_IS_TRUE(resume != writes.end());
        const auto& injected = (*resume)["params"]["config"]["shell_environment_policy"]["set"];
        VERIFY_ARE_EQUAL(std::string{ "run-capability" }, injected["RICHER_TASK_CAPABILITY"].asString());
        VERIFY_ARE_EQUAL(std::string{ RunId }, injected["RICHER_TASK_RUN_ID"].asString());
        adapter.Stop();
        VERIFY_IS_TRUE(state->Stopped);
    }

    void CodexAppServerAdapterTests::TranslatesEventsAndCorrelatesApprovalResponse()
    {
        const auto state = std::make_shared<FakeCodexAppServerState>();
        CodexAppServerAdapter adapter{ MakeFakeCodexAppServerTransportFactory(state) };
        VERIFY_IS_TRUE(adapter.ProbeCapabilities(nullptr).Succeeded());

        Json::Value delta{ Json::objectValue };
        delta["method"] = "item/agentMessage/delta";
        delta["params"]["threadId"] = "provider-thread-1";
        delta["params"]["turnId"] = "provider-turn-1";
        delta["params"]["itemId"] = "item-1";
        delta["params"]["delta"] = "hello";
        state->Queue(delta);
        auto event = adapter.NextEvent(100, nullptr);
        VERIFY_IS_TRUE(event.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "hello" }, event.Value->Text);

        Json::Value approval{ Json::objectValue };
        approval["id"] = Json::UInt64{ 99 };
        approval["method"] = "item/fileChange/requestApproval";
        approval["params"]["threadId"] = "provider-thread-1";
        approval["params"]["turnId"] = "provider-turn-1";
        approval["params"]["itemId"] = "approval-item";
        approval["params"]["reason"] = "Apply patch";
        state->Queue(approval);
        event = adapter.NextEvent(100, nullptr);
        VERIFY_IS_TRUE(event.Succeeded());
        VERIFY_IS_TRUE(event.Value->Kind == TaskProviderEventKind::ApprovalRequested);
        VERIFY_ARE_EQUAL(std::string{ "approval-item" }, event.Value->ApprovalId);
        VERIFY_IS_TRUE(adapter.RespondToApproval(
            "provider-thread-1", "provider-turn-1", "approval-item",
            TaskProviderApprovalDecision::Approved, nullptr).Succeeded());

        const auto writes = state->SnapshotWrites();
        const auto response = std::find_if(writes.begin(), writes.end(), [](const auto& value) {
            return value.isMember("id") && value["id"].isUInt64() && value["id"].asUInt64() == 99 &&
                   value.isMember("result");
        });
        VERIFY_IS_TRUE(response != writes.end());
        VERIFY_ARE_EQUAL(std::string{ "accept" }, (*response)["result"]["decision"].asString());
        VERIFY_IS_TRUE(adapter.RespondToApproval(
            "provider-thread-1", "provider-turn-1", "approval-item",
            TaskProviderApprovalDecision::Denied, nullptr).Status == TaskProviderStatus::NotFound);

        event = adapter.NextEvent(1, nullptr);
        VERIFY_IS_TRUE(event.Succeeded());
        VERIFY_IS_TRUE(event.NoEvent);
    }

    void CodexAppServerAdapterTests::YoloPrecedesAppServerSubcommandInProcessCommandLine()
    {
        const auto commandLine = BuildCodexAppServerCommandLine(
            LR"(C:\Program Files\Codex\codex.exe)");
        VERIFY_ARE_EQUAL(
            std::wstring{ LR"("C:\Program Files\Codex\codex.exe" --yolo app-server --listen stdio://)" },
            commandLine);
    }

    void CodexAppServerAdapterTests::PropagatesMethodErrorsAndTransportFailure()
    {
        const auto state = std::make_shared<FakeCodexAppServerState>();
        state->RpcErrors.emplace("thread/start", -32601);
        CodexAppServerAdapter adapter{ MakeFakeCodexAppServerTransportFactory(state) };
        TaskProviderSessionRequest request;
        request.TaskId = TaskId;
        request.WorkingDirectory = LR"(C:\work\task)";
        const auto started = adapter.StartSession(request, nullptr);
        VERIFY_IS_TRUE(started.Status == TaskProviderStatus::UnsupportedCapability);

        state->EmptyReadKind = CodexTransportReadKind::EndOfStream;
        const auto event = adapter.NextEvent(10, nullptr);
        VERIFY_IS_TRUE(event.Status == TaskProviderStatus::TransportClosed);
    }

    void CodexAppServerAdapterTests::OptInInstalledCodexInitializeSmoke()
    {
        std::array<wchar_t, 32'768> executable{};
        const auto length = GetEnvironmentVariableW(
            L"RICHER_RUN_CODEX_PROVIDER_SMOKE",
            executable.data(),
            static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size())
        {
            WEX::Logging::Log::Comment(L"RICHER_RUN_CODEX_PROVIDER_SMOKE is unset; installed Codex smoke is opt-in.");
            return;
        }
        const std::filesystem::path path{ executable.data(), executable.data() + length };
        auto adapter = MakeCodexAppServerAdapter(path);
        VERIFY_IS_NOT_NULL(adapter.get());
        const auto capabilities = adapter->ProbeCapabilities(nullptr);
        VERIFY_IS_TRUE(capabilities.Succeeded());
        VERIFY_IS_TRUE(capabilities.Value.has_value());
        VERIFY_ARE_EQUAL(std::string{ "codex" }, capabilities.Value->Provider);
        adapter->Stop();
    }
}
