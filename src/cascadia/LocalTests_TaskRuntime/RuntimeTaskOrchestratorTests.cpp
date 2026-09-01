// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRuntimeRtaskCapabilityTokenSource.h"
#include "RuntimeTaskOrchestrator.h"
#include "RuntimeTaskProviderSink.h"
#include "RuntimeTaskSchedulingPolicy.h"
#include "TaskCoreStoreTestFixtures.h"
#include "TaskRuntimeTestFixtures.h"

#include <deque>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskProvider;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskCoreLocalTests::TestFixtures;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        class CompletingProviderAdapter final : public ITaskProviderAdapter
        {
        public:
            TaskProviderCapabilitiesResult ProbeCapabilities(HANDLE) override
            {
                TaskProviderCapabilitiesResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = TaskProviderCapabilities{
                    "codex", "test", 2, 0, RequiredTaskProviderCapabilities
                };
                return result;
            }

            TaskProviderSessionResult StartSession(const TaskProviderSessionRequest&, HANDLE) override
            {
                TaskProviderSessionResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = TaskProviderSession{ "test-thread", TaskProviderSessionState::Ready };
                return result;
            }

            TaskProviderSessionResult ResumeSession(const std::string_view, HANDLE) override
            {
                TaskProviderSessionResult result;
                result.Status = TaskProviderStatus::NotFound;
                result.Error = "no prior session";
                return result;
            }

            TaskProviderRunResult StartRun(const TaskProviderRunRequest&, HANDLE) override
            {
                std::scoped_lock lock{ _mutex };
                TaskProviderEvent event;
                event.Sequence = ++_sequence;
                event.Kind = TaskProviderEventKind::RunCompleted;
                event.SessionId = "test-thread";
                event.RunId = "test-turn";
                event.Text = "done";
                _events.emplace_back(std::move(event));
                TaskProviderRunResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = TaskProviderRun{ "test-thread", "test-turn", TaskProviderRunState::Running };
                return result;
            }

            TaskProviderResult SteerRun(std::string_view, std::string_view, std::string_view, HANDLE) override { return { TaskProviderStatus::Succeeded, {} }; }
            TaskProviderResult InterruptRun(std::string_view, std::string_view, HANDLE) override { return { TaskProviderStatus::Succeeded, {} }; }
            TaskProviderResult RespondToApproval(std::string_view, std::string_view, std::string_view, TaskProviderApprovalDecision, HANDLE) override { return { TaskProviderStatus::Succeeded, {} }; }
            TaskProviderResult SuspendSession(std::string_view, HANDLE) override { return { TaskProviderStatus::Succeeded, {} }; }
            TaskProviderResult CloseSessionProjection(std::string_view, HANDLE) override { return { TaskProviderStatus::Succeeded, {} }; }

            TaskProviderEventResult NextEvent(const uint32_t timeoutMilliseconds, const HANDLE cancellationEvent) override
            {
                {
                    const std::scoped_lock lock{ _mutex };
                    if (!_events.empty())
                    {
                        TaskProviderEventResult result;
                        result.Status = TaskProviderStatus::Succeeded;
                        result.Value = std::move(_events.front());
                        _events.pop_front();
                        return result;
                    }
                }
                if (cancellationEvent && WaitForSingleObject(cancellationEvent, timeoutMilliseconds) == WAIT_OBJECT_0)
                {
                    TaskProviderEventResult result;
                    result.Status = TaskProviderStatus::Cancelled;
                    result.Error = "stopped";
                    return result;
                }
                TaskProviderEventResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.NoEvent = true;
                return result;
            }

            TaskProviderFailureDisposition ClassifyFailureAndResumeSafety(TaskProviderStatus, std::string_view) const noexcept override
            {
                return {};
            }
            void Stop() noexcept override {}

        private:
            std::mutex _mutex;
            std::deque<TaskProviderEvent> _events;
            uint64_t _sequence{};
        };

        std::string _utf8(const std::wstring_view value)
        {
            const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<size_t>(count), '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
            return result;
        }

        bool _waitUntil(const std::function<bool()>& predicate, const uint32_t timeout = 10'000)
        {
            const auto deadline = GetTickCount64() + timeout;
            while (GetTickCount64() < deadline)
            {
                if (predicate())
                    return true;
                Sleep(20);
            }
            return predicate();
        }
    }

    class RuntimeTaskOrchestratorTests
    {
        TEST_CLASS(RuntimeTaskOrchestratorTests);

        TEST_METHOD(RetryPolicyIsAllowlistedBoundedAndBackedOff);
        TEST_METHOD(FollowupPromptPreservesCanonicalMessageOrder);
        TEST_METHOD(DirectQueuedRunExecutesToCompletionAndReleasesRuntimeResources);
        TEST_METHOD(StopIntentCancelsPendingRunAndRetainsManualHold);
    };

    void RuntimeTaskOrchestratorTests::RetryPolicyIsAllowlistedBoundedAndBackedOff()
    {
        auto task = MakeCreateTaskCommand();
        auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100).Run;
        run.State = TaskRunState::Interrupted;
        run.ReasonCode = "runtime_recovery";
        auto decision = EvaluateRuntimeTaskAutomaticRetry(run, { run });
        VERIFY_IS_TRUE(decision.Retry);
        VERIFY_ARE_EQUAL(uint32_t{ 2 }, decision.MaximumExecutions);
        VERIFY_ARE_EQUAL(uint32_t{ 1000 }, decision.BackoffMilliseconds);

        auto retry = run;
        retry.Id = MakeId<TaskRunId>(3);
        retry.RetryOfRunId = run.Id;
        retry.Trigger = TaskRunTrigger::AutomaticRetry;
        decision = EvaluateRuntimeTaskAutomaticRetry(retry, { run, retry });
        VERIFY_IS_FALSE(decision.Retry);
        run.ReasonCode = "authentication";
        VERIFY_IS_FALSE(EvaluateRuntimeTaskAutomaticRetry(run, { run }).Retry);
    }

    void RuntimeTaskOrchestratorTests::FollowupPromptPreservesCanonicalMessageOrder()
    {
        auto task = MakeCreateTaskCommand();
        auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100).Run;
        TaskAggregateSnapshot aggregate;
        aggregate.Task = task.Task;
        aggregate.RunMessages = {
            { run.Id, MakeId<TaskMessageId>(2), 1, 1, "second" },
            { run.Id, MakeId<TaskMessageId>(1), 0, 1, "first" },
        };
        const auto prompt = BuildRuntimeTaskPrompt(aggregate, run);
        VERIFY_IS_TRUE(prompt.has_value());
        VERIFY_IS_TRUE(prompt->find("first") < prompt->find("second"));
        aggregate.Task.ManualHold = true;
        aggregate.Messages.push_back({ MakeId<TaskMessageId>(4), task.Task.Id, TaskMessageAuthor::User, TaskMessageState::Deferred, 1, "later" });
        VERIFY_IS_FALSE(ShouldClaimRuntimeTaskMessages(aggregate));
        aggregate.Task.ManualHold = false;
        VERIFY_IS_TRUE(ShouldClaimRuntimeTaskMessages(aggregate));
    }

    void RuntimeTaskOrchestratorTests::DirectQueuedRunExecutesToCompletionAndReleasesRuntimeResources()
    {
        TemporaryDatabasePath database;
        UniqueRuntimeRoot runtimeRoot;
        std::filesystem::create_directory(runtimeRoot.Path());
        const auto store = std::make_shared<SqliteTaskStore>(database.Path());
        auto task = MakeCreateTaskCommand();
        task.ExecutionContext.Mode = ExecutionMode::Direct;
        task.ExecutionContext.SourceRoot = _utf8(runtimeRoot.Path().native());
        task.ExecutionContext.RuntimeRoot = task.ExecutionContext.SourceRoot;
        VERIFY_IS_TRUE(store->CreateTask(task).Succeeded());
        auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        VERIFY_IS_TRUE(store->CreateTaskRun(run).Succeeded());

        const auto identity = RequireIdentity(runtimeRoot.Path());
        auto terminals = std::make_shared<RuntimeTaskTerminalService>(
            identity,
            "b1000000-0000-4000-8000-000000000001",
            [](auto&&, auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; },
            [](auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; });
        auto sink = std::make_shared<RuntimeTaskProviderSink>(store, terminals);
        auto provider = std::make_shared<RuntimeTaskProviderService>(
            [] { return std::make_unique<CompletingProviderAdapter>(); }, sink);
        auto tokens = std::make_shared<FakeRuntimeRtaskCapabilityTokenSource>();
        tokens->Tokens.emplace_back(std::string(64, 'a'));
        auto registry = std::make_shared<RuntimeRtaskCapabilityRegistry>(
            RuntimeRtaskCapabilityRegistryConfiguration{}, tokens);
        auto rtask = std::make_shared<RuntimeRtaskCommandService>(store, registry);
        auto module = std::filesystem::path{ LR"(C:\test\rtask.exe)" };
        RuntimeTaskOrchestratorConfiguration configuration;
        configuration.ActivePollMilliseconds = 50;
        configuration.IdlePollMilliseconds = 100;
        configuration.TerminalIdleMilliseconds = 60'000;
        RuntimeTaskOrchestrator orchestrator{
            store, provider, rtask, terminals, runtimeRoot.Path(), {}, module, R"(\\.\pipe\orchestrator-test)", configuration
        };
        VERIFY_IS_TRUE(orchestrator.Valid());
        VERIFY_IS_TRUE(orchestrator.Start());
        orchestrator.Notify();
        VERIFY_IS_TRUE(_waitUntil([&] {
            const auto aggregate = store->LoadTask(task.Task.Id);
            return aggregate && !aggregate->Runs.empty() && aggregate->Runs.front().State == TaskRunState::Completed;
        }));
        VERIFY_IS_TRUE(_waitUntil([&] {
            const auto aggregate = store->LoadTask(task.Task.Id);
            return aggregate && aggregate->Terminal.State == TaskTerminalState::Ready;
        }));
        orchestrator.Stop();
        provider->Stop();
        terminals->Stop();
        const auto aggregate = store->LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(aggregate.has_value());
        VERIFY_IS_TRUE(aggregate->Runs.front().State == TaskRunState::Completed);
    }

    void RuntimeTaskOrchestratorTests::StopIntentCancelsPendingRunAndRetainsManualHold()
    {
        TemporaryDatabasePath database;
        UniqueRuntimeRoot runtimeRoot;
        std::filesystem::create_directory(runtimeRoot.Path());
        const auto store = std::make_shared<SqliteTaskStore>(database.Path());
        auto task = MakeCreateTaskCommand();
        task.ExecutionContext.SourceRoot = _utf8(runtimeRoot.Path().native());
        task.ExecutionContext.RuntimeRoot = task.ExecutionContext.SourceRoot;
        VERIFY_IS_TRUE(store->CreateTask(task).Succeeded());
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        VERIFY_IS_TRUE(store->CreateTaskRun(run).Succeeded());
        VERIFY_IS_TRUE(store->ChangeTaskRunState(
                                MakeRunStateCommand(run, 3, 2, 1200, TaskRunState::Preparing))
                           .Succeeded());
        VERIFY_IS_TRUE(store->ChangeTaskRunState(
                                MakeRunStateCommand(run, 4, 3, 1300, TaskRunState::WaitingForDirectory))
                           .Succeeded());
        const auto intent = MakeStartIntentCommand(task, 1, 5, 4, 1400, TaskIntentKind::Stop);
        VERIFY_IS_TRUE(store->StartTaskIntent(intent).Succeeded());

        const auto identity = RequireIdentity(runtimeRoot.Path());
        auto terminals = std::make_shared<RuntimeTaskTerminalService>(
            identity, "b2000000-0000-4000-8000-000000000002", [](auto&&, auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; }, [](auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; });
        auto sink = std::make_shared<RuntimeTaskProviderSink>(store, terminals);
        auto provider = std::make_shared<RuntimeTaskProviderService>(
            [] { return std::make_unique<CompletingProviderAdapter>(); }, sink);
        auto tokens = std::make_shared<FakeRuntimeRtaskCapabilityTokenSource>();
        auto registry = std::make_shared<RuntimeRtaskCapabilityRegistry>(
            RuntimeRtaskCapabilityRegistryConfiguration{}, tokens);
        auto rtask = std::make_shared<RuntimeRtaskCommandService>(store, registry);
        RuntimeTaskOrchestratorConfiguration configuration;
        configuration.ActivePollMilliseconds = 50;
        configuration.IdlePollMilliseconds = 100;
        configuration.TerminalIdleMilliseconds = 60'000;
        RuntimeTaskOrchestrator orchestrator{
            store, provider, rtask, terminals, runtimeRoot.Path(), {}, LR"(C:\test\rtask.exe)", R"(\\.\pipe\orchestrator-stop-test)", configuration
        };
        VERIFY_IS_TRUE(orchestrator.Start());
        orchestrator.Notify();
        VERIFY_IS_TRUE(_waitUntil([&] {
            const auto aggregate = store->LoadTask(task.Task.Id);
            return aggregate && aggregate->Runs.front().State == TaskRunState::Cancelled &&
                   aggregate->Intent && aggregate->Intent->Stage == TaskIntentStage::Completed;
        }));
        orchestrator.Stop();
        provider->Stop();
        terminals->Stop();
        const auto aggregate = store->LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(aggregate->Task.ManualHold);
    }
}
