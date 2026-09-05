// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "CodexAppServerAdapter.h"
#include "CodexAppServerTransport.h"
#include "FakeCodexAppServerTransport.h"
#include <thread>

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

        bool _waitWrites(const std::shared_ptr<FakeCodexAppServerState>& state, const size_t count)
        {
            const auto deadline = GetTickCount64() + 1000;
            while (state->SnapshotWrites().size() < count && GetTickCount64() < deadline) Sleep(5);
            return state->SnapshotWrites().size() >= count;
        }
    }

    class CodexAppServerAdapterTests
    {
        TEST_CLASS(CodexAppServerAdapterTests);

        TEST_METHOD(InitializesStartsResumesAndRunsWithScopedContext);
        TEST_METHOD(InitialRunDoesNotResumeMissingRolloutAndNextRunRefreshesCapability);
        TEST_METHOD(CloseUnloadsOnlyItsThreadAndRetriesHistoryRestoration);
        TEST_METHOD(TranslatesEventsAndCorrelatesApprovalResponse);
        TEST_METHOD(PropagatesMethodErrorsAndTransportFailure);
        TEST_METHOD(YoloPrecedesAppServerSubcommandInProcessCommandLine);
        TEST_METHOD(OptInInstalledCodexInitializeSmoke);
        TEST_METHOD(ConcurrentRpcRequestsCorrelateOutOfOrderReplies);
        TEST_METHOD(RpcTimeoutAndCancellationDoNotPoisonPeers);
        TEST_METHOD(RpcStopWakesPendingRequestsAndAllowsRestart);
        TEST_METHOD(StoppedAdapterCannotInitializeAgain);
    };

    void CodexAppServerAdapterTests::StoppedAdapterCannotInitializeAgain()
    {
        const auto state = std::make_shared<FakeCodexAppServerState>();
        state->SuppressedResponses = { "initialize" };
        CodexAppServerAdapter adapter{ MakeFakeCodexAppServerTransportFactory(state) };
        TaskProviderCapabilitiesResult initialized;
        std::thread worker{ [&] { initialized = adapter.ProbeCapabilities(nullptr); } };
        const auto finish = wil::scope_exit([&] { adapter.Stop(); if (worker.joinable()) worker.join(); });
        VERIFY_IS_TRUE(_waitWrites(state, 1));
        adapter.Stop();
        worker.join();
        VERIFY_IS_FALSE(initialized.Succeeded());
        const auto writes = state->SnapshotWrites().size();
        VERIFY_IS_TRUE(adapter.ProbeCapabilities(nullptr).Status == TaskProviderStatus::TransportClosed);
        VERIFY_IS_TRUE(adapter.StartRun(_runRequest(), nullptr).Status == TaskProviderStatus::TransportClosed);
        VERIFY_ARE_EQUAL(writes, state->SnapshotWrites().size());
        VERIFY_IS_TRUE(state->Stopped);
    }

    void CodexAppServerAdapterTests::ConcurrentRpcRequestsCorrelateOutOfOrderReplies()
    {
        const auto state = std::make_shared<FakeCodexAppServerState>();
        state->SuppressedResponses = { "slow", "fast" };
        CodexAppServerJsonRpc rpc{ MakeFakeCodexAppServerTransportFactory(state) };
        VERIFY_IS_TRUE(rpc.Start().Succeeded());
        wil::unique_handle fastDone{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        wil::unique_handle slowDone{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        CodexJsonRpcResult slow, fast;
        std::thread slowThread{ [&] {
            slow = rpc.Request("slow", Json::Value{ Json::objectValue }, 5000, nullptr);
            SetEvent(slowDone.get());
        } };
        const auto stopSlow = wil::scope_exit([&] { rpc.Stop(); if (slowThread.joinable()) slowThread.join(); });
        VERIFY_IS_TRUE(_waitWrites(state, 1));
        std::thread fastThread{ [&] {
            fast = rpc.Request("fast", Json::Value{ Json::objectValue }, 5000, nullptr);
            SetEvent(fastDone.get());
        } };
        const auto stopFast = wil::scope_exit([&] { rpc.Stop(); if (fastThread.joinable()) fastThread.join(); });
        VERIFY_IS_TRUE(_waitWrites(state, 2));
        const auto writes = state->SnapshotWrites();
        Json::Value reply{ Json::objectValue };
        reply["id"] = writes[1]["id"];
        reply["result"]["owner"] = "fast";
        state->Queue(reply);
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(fastDone.get(), 1000));
        VERIFY_ARE_EQUAL(DWORD{ WAIT_TIMEOUT }, WaitForSingleObject(slowDone.get(), 0));
        reply["id"] = writes[0]["id"];
        reply["result"]["owner"] = "slow";
        state->Queue(reply);
        slowThread.join();
        fastThread.join();
        VERIFY_IS_TRUE(slow.Succeeded());
        VERIFY_IS_TRUE(fast.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "slow" }, slow.Value["owner"].asString());
        VERIFY_ARE_EQUAL(std::string{ "fast" }, fast.Value["owner"].asString());
    }

    void CodexAppServerAdapterTests::RpcTimeoutAndCancellationDoNotPoisonPeers()
    {
        const auto state = std::make_shared<FakeCodexAppServerState>();
        state->SuppressedResponses = { "slow" };
        CodexAppServerJsonRpc rpc{ MakeFakeCodexAppServerTransportFactory(state) };
        VERIFY_IS_TRUE(rpc.Start().Succeeded());
        VERIFY_IS_TRUE(rpc.Request("slow", Json::Value{ Json::objectValue }, 20, nullptr).Status == TaskProviderStatus::Timeout);
        wil::unique_handle cancel{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        CodexJsonRpcResult cancelled;
        std::thread worker{ [&] { cancelled = rpc.Request("slow", Json::Value{ Json::objectValue }, 5000, cancel.get()); } };
        const auto stop = wil::scope_exit([&] { rpc.Stop(); if (worker.joinable()) worker.join(); });
        VERIFY_IS_TRUE(_waitWrites(state, 2));
        SetEvent(cancel.get());
        worker.join();
        VERIFY_IS_TRUE(cancelled.Status == TaskProviderStatus::Cancelled);
        for (const auto& request : state->SnapshotWrites())
        {
            Json::Value reply{ Json::objectValue };
            reply["id"] = request["id"];
            reply["result"] = Json::Value{ Json::objectValue };
            state->Queue(reply);
        }
        VERIFY_IS_TRUE(rpc.Request("fast", Json::Value{ Json::objectValue }, 1000, nullptr).Succeeded());
        VERIFY_IS_TRUE(rpc.Running());
    }

    void CodexAppServerAdapterTests::RpcStopWakesPendingRequestsAndAllowsRestart()
    {
        const auto state = std::make_shared<FakeCodexAppServerState>();
        state->SuppressedResponses = { "slow" };
        CodexAppServerJsonRpc rpc{ MakeFakeCodexAppServerTransportFactory(state) };
        VERIFY_IS_TRUE(rpc.Start().Succeeded());
        CodexJsonRpcResult pending;
        std::thread worker{ [&] { pending = rpc.Request("slow", Json::Value{ Json::objectValue }, 5000, nullptr); } };
        const auto stop = wil::scope_exit([&] { rpc.Stop(); if (worker.joinable()) worker.join(); });
        VERIFY_IS_TRUE(_waitWrites(state, 1));
        const auto start = GetTickCount64();
        rpc.Stop();
        VERIFY_IS_TRUE(rpc.Start().Succeeded());
        VERIFY_IS_TRUE(rpc.Request("fast", Json::Value{ Json::objectValue }, 1000, nullptr).Succeeded());
        worker.join();
        VERIFY_IS_TRUE(GetTickCount64() - start < 1000);
        VERIFY_IS_TRUE(pending.Status == TaskProviderStatus::TransportClosed);
    }

    void CodexAppServerAdapterTests::InitialRunDoesNotResumeMissingRolloutAndNextRunRefreshesCapability()
    {
        const auto state = std::make_shared<FakeCodexAppServerState>();
        state->RpcErrors.emplace("thread/resume", -32600);
        CodexAppServerAdapter adapter{ MakeFakeCodexAppServerTransportFactory(state) };
        auto request = _runRequest();
        const auto session = adapter.StartSession(
            { std::string{ TaskId }, request.Context.WorkingDirectory, request.Context.Model, request.Context }, nullptr);
        VERIFY_IS_TRUE(session.Succeeded());
        request.SessionId = session.Value->SessionId;
        VERIFY_IS_TRUE(adapter.StartRun(request, nullptr).Succeeded());
        auto writes = state->SnapshotWrites();
        VERIFY_IS_FALSE(std::ranges::any_of(writes, [](const auto& value) { return value["method"] == "thread/resume"; }));
        const auto start = std::ranges::find_if(writes, [](const auto& value) { return value["method"] == "thread/start"; });
        VERIFY_IS_TRUE(start != writes.end());
        VERIFY_ARE_EQUAL(std::string{ "never" }, (*start)["params"]["approvalPolicy"].asString());
        VERIFY_ARE_EQUAL(std::string{ "danger-full-access" }, (*start)["params"]["sandbox"].asString());
        VERIFY_ARE_EQUAL(std::string{ "run-capability" }, (*start)["params"]["config"]["shell_environment_policy"]["set"]["RICHER_TASK_CAPABILITY"].asString());

        state->RpcErrors.erase("thread/resume");
        request.Context.RunId = "20000000-0000-4000-8000-000000000003";
        request.Context.RtaskCapability = "next-run-capability";
        VERIFY_IS_TRUE(adapter.StartRun(request, nullptr).Succeeded());
        writes = state->SnapshotWrites();
        const auto resume = std::ranges::find_if(writes, [](const auto& value) { return value["method"] == "thread/resume"; });
        VERIFY_IS_TRUE(resume != writes.end());
        VERIFY_ARE_EQUAL(std::string{ "never" }, (*resume)["params"]["approvalPolicy"].asString());
        VERIFY_ARE_EQUAL(std::string{ "danger-full-access" }, (*resume)["params"]["sandbox"].asString());
        VERIFY_ARE_EQUAL(request.Context.RtaskCapability, (*resume)["params"]["config"]["shell_environment_policy"]["set"]["RICHER_TASK_CAPABILITY"].asString());
        VERIFY_IS_TRUE(std::ranges::any_of(writes.begin(), resume, [](const auto& value) { return value["method"] == "thread/archive"; }));
        VERIFY_IS_TRUE(std::ranges::any_of(writes.begin(), resume, [](const auto& value) { return value["method"] == "thread/unarchive"; }));
    }

    void CodexAppServerAdapterTests::CloseUnloadsOnlyItsThreadAndRetriesHistoryRestoration()
    {
        const auto state = std::make_shared<FakeCodexAppServerState>();
        CodexAppServerAdapter adapter{ MakeFakeCodexAppServerTransportFactory(state) };
        VERIFY_IS_TRUE(adapter.ProbeCapabilities(nullptr).Succeeded());
        state->RpcErrors.emplace("thread/unarchive", -32000);
        VERIFY_IS_FALSE(adapter.CloseSessionProjection("provider-thread-1", nullptr).Succeeded());
        state->RpcErrors.erase("thread/unarchive");
        VERIFY_IS_TRUE(adapter.CloseSessionProjection("provider-thread-1", nullptr).Succeeded());
        const auto writes = state->SnapshotWrites();
        VERIFY_ARE_EQUAL(ptrdiff_t{ 1 }, std::ranges::count_if(writes, [](const auto& value) { return value["method"] == "thread/archive"; }));
        for (const auto& value : writes)
        {
            if (value["method"] == "thread/archive" || value["method"] == "thread/unarchive")
                VERIFY_ARE_EQUAL(std::string{ "provider-thread-1" }, value["params"]["threadId"].asString());
        }
        VERIFY_IS_FALSE(state->Stopped);
        state->RpcErrors.emplace("thread/unarchive", -32000);
        VERIFY_IS_FALSE(adapter.CloseSessionProjection("provider-thread-1", nullptr).Succeeded());
        state->RpcErrors.erase("thread/unarchive");
        VERIFY_IS_TRUE(adapter.ResumeSession("provider-thread-1", nullptr).Succeeded());
        VERIFY_IS_TRUE(adapter.CloseSessionProjection("provider-thread-1", nullptr).Succeeded());
        VERIFY_ARE_EQUAL(ptrdiff_t{ 3 }, std::ranges::count_if(state->SnapshotWrites(), [](const auto& value) { return value["method"] == "thread/archive"; }));
    }

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
