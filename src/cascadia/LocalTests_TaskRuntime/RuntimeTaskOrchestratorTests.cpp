// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRuntimeRtaskCapabilityTokenSource.h"
#include "RuntimeTaskOrchestrator.h"
#include "RuntimeTaskProviderSink.h"
#include "RuntimeTaskSchedulingPolicy.h"
#include "TaskCoreStoreTestFixtures.h"
#include "TaskRuntimeTestFixtures.h"
#include "TaskGitTestSupport.h"
#include "RuntimeTaskProjectionService.h"
#include "DirectDirectoryLease.h"

#include <deque>
#include <wil/resource.h>

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
        struct OrchestratorProviderState
        {
            bool CompleteOnStart{ true };
            std::atomic<TaskProviderStatus> InterruptStatus{ TaskProviderStatus::Succeeded };
            std::atomic_uint32_t InterruptCalls{};
            std::atomic<TaskProviderEventKind> TerminalEvent{ TaskProviderEventKind::UnknownOptional };
        };

        class CompletingProviderAdapter final : public ITaskProviderAdapter
        {
        public:
            explicit CompletingProviderAdapter(std::shared_ptr<OrchestratorProviderState> state = std::make_shared<OrchestratorProviderState>()) :
                _state{ std::move(state) } {}

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
                if (_state->CompleteOnStart)
                    _events.emplace_back(std::move(event));
                TaskProviderRunResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = TaskProviderRun{ "test-thread", "test-turn", TaskProviderRunState::Running };
                return result;
            }

            TaskProviderResult SteerRun(std::string_view, std::string_view, std::string_view, HANDLE) override { return { TaskProviderStatus::Succeeded, {} }; }
            TaskProviderResult InterruptRun(std::string_view, std::string_view, HANDLE) override
            {
                ++_state->InterruptCalls;
                const auto status = _state->InterruptStatus.load();
                return { status, status == TaskProviderStatus::Succeeded ? "" : "interrupt rejected" };
            }
            TaskProviderResult RespondToApproval(std::string_view, std::string_view, std::string_view, TaskProviderApprovalDecision, HANDLE) override { return { TaskProviderStatus::Succeeded, {} }; }
            TaskProviderResult SuspendSession(std::string_view, HANDLE) override { return { TaskProviderStatus::Succeeded, {} }; }
            TaskProviderResult CloseSessionProjection(std::string_view, HANDLE) override { return { TaskProviderStatus::Succeeded, {} }; }

            TaskProviderEventResult NextEvent(const uint32_t timeoutMilliseconds, const HANDLE cancellationEvent) override
            {
                {
                    const std::scoped_lock lock{ _mutex };
                    if (const auto terminal = _state->TerminalEvent.exchange(TaskProviderEventKind::UnknownOptional);
                        terminal != TaskProviderEventKind::UnknownOptional)
                    {
                        TaskProviderEvent event;
                        event.Sequence = ++_sequence;
                        event.Kind = terminal;
                        event.SessionId = "test-thread";
                        event.RunId = "test-turn";
                        _events.push_back(std::move(event));
                    }
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
            std::shared_ptr<OrchestratorProviderState> _state;
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

        struct OrchestratorFixture
        {
            TaskGitLocalTests::TestSupport::TemporaryDirectory Root;
            TemporaryDatabasePath Database;
            std::shared_ptr<SqliteTaskStore> Store{ std::make_shared<SqliteTaskStore>(Database.Path()) };
            std::shared_ptr<OrchestratorProviderState> State{ std::make_shared<OrchestratorProviderState>() };
            std::shared_ptr<RuntimeTaskTerminalService> Terminals;
            std::shared_ptr<RuntimeTaskProviderService> Provider;
            std::shared_ptr<RuntimeRtaskCommandService> Rtask;
            std::unique_ptr<RuntimeTaskOrchestrator> Orchestrator;

            explicit OrchestratorFixture(std::filesystem::path git = {})
            {
                Terminals = std::make_shared<RuntimeTaskTerminalService>(
                    RequireIdentity(Root.Path()), "b4000000-0000-4000-8000-000000000004",
                    [](auto&&, auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; },
                    [](auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; });
                Provider = std::make_shared<RuntimeTaskProviderService>(
                    [state = State] { return std::make_unique<CompletingProviderAdapter>(state); },
                    std::make_shared<RuntimeTaskProviderSink>(Store, Terminals));
                auto tokens = std::make_shared<FakeRuntimeRtaskCapabilityTokenSource>();
                for (char value = 'a'; value <= 'd'; ++value)
                    tokens->Tokens.emplace_back(std::string(64, value));
                Rtask = std::make_shared<RuntimeRtaskCommandService>(Store,
                    std::make_shared<RuntimeRtaskCapabilityRegistry>(RuntimeRtaskCapabilityRegistryConfiguration{}, tokens));
                RuntimeTaskOrchestratorConfiguration configuration;
                configuration.ActivePollMilliseconds = 50;
                configuration.IdlePollMilliseconds = 100;
                configuration.TerminalIdleMilliseconds = 60'000;
                Orchestrator = std::make_unique<RuntimeTaskOrchestrator>(
                    Store, Provider, Rtask, Terminals, Root.Path(), std::move(git),
                    LR"(C:\test\rtask.exe)", R"(\\.\pipe\orchestrator-regression)", configuration);
            }

            ~OrchestratorFixture()
            {
                Orchestrator->Stop();
                Provider->Stop();
                Terminals->Stop();
            }

            CreateTaskCommand CreateDirectTask()
            {
                auto task = MakeCreateTaskCommand();
                task.ExecutionContext.Mode = ExecutionMode::Direct;
                task.ExecutionContext.SourceRoot = _utf8(Root.Path().native());
                task.ExecutionContext.RuntimeRoot = task.ExecutionContext.SourceRoot;
                VERIFY_IS_TRUE(Store->CreateTask(task).Succeeded());
                return task;
            }
        };
    }

    class RuntimeTaskOrchestratorTests
    {
        TEST_CLASS(RuntimeTaskOrchestratorTests);

        TEST_METHOD(RetryPolicyIsAllowlistedBoundedAndBackedOff);
        TEST_METHOD(FollowupPromptPreservesCanonicalMessageOrder);
        TEST_METHOD(ManualAndAutomaticRetriesRetainFollowupInput);
        TEST_METHOD(DirectQueuedRunExecutesToCompletionAndReleasesRuntimeResources);
        TEST_METHOD(StopIntentCancelsPendingRunAndRetainsManualHold);
        TEST_METHOD(IdleTerminalWithoutProviderSessionSuspends);
        TEST_METHOD(RecoversEveryPreparationStateWithoutAnAgentSession);
        TEST_METHOD(SchedulerShutdownPreservesDirectoryWaitForRestart);
        TEST_METHOD(StopCloseAndDeleteWaitForProviderCompletionAndKeepDirectoryLease);
        TEST_METHOD(DeleteCancelsAnActualDirectoryWait);
        TEST_METHOD(WorktreeTaskCreatedThroughProjectionRunsToCompletion);
    };

    void RuntimeTaskOrchestratorTests::RecoversEveryPreparationStateWithoutAnAgentSession()
    {
        for (const auto state : { TaskRunState::Preparing, TaskRunState::WaitingForDirectory, TaskRunState::Starting })
        {
            OrchestratorFixture fixture;
            const auto task = fixture.CreateDirectTask();
            const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
            VERIFY_IS_TRUE(fixture.Store->CreateTaskRun(run).Succeeded());
            VERIFY_IS_TRUE(fixture.Store->ChangeTaskRunState(MakeRunStateCommand(run, 3, 2, 1200, TaskRunState::Preparing)).Succeeded());
            if (state != TaskRunState::Preparing)
                VERIFY_IS_TRUE(fixture.Store->ChangeTaskRunState(MakeRunStateCommand(run, 4, 3, 1300, state)).Succeeded());
            VERIFY_IS_TRUE(fixture.Orchestrator->Start());
            VERIFY_IS_TRUE(_waitUntil([&] {
                const auto aggregate = fixture.Store->LoadTask(task.Task.Id);
                return aggregate && aggregate->Runs.size() == 1 && aggregate->Runs.front().State == TaskRunState::Completed;
            }));
        }
    }

    void RuntimeTaskOrchestratorTests::SchedulerShutdownPreservesDirectoryWaitForRestart()
    {
        using namespace Microsoft::Terminal::TaskExecution;
        OrchestratorFixture fixture;
        const auto task = fixture.CreateDirectTask();
        const auto directory = ResolveCanonicalDirectoryIdentity(fixture.Root.Path(), DirectoryIdentityPolicy::Direct);
        VERIFY_IS_TRUE(directory.Succeeded());
        DirectDirectoryLeaseManager owner;
        wil::unique_handle cancellation{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        auto lease = owner.Acquire(*directory.Identity, cancellation.get());
        VERIFY_IS_TRUE(lease.Acquired());
        VERIFY_IS_TRUE(fixture.Store->CreateTaskRun(MakeCreateRunCommand(task, 1, 2, 1, 1100)).Succeeded());
        VERIFY_IS_TRUE(fixture.Orchestrator->Start());
        VERIFY_IS_TRUE(_waitUntil([&] { return owner.ParticipantCount(*directory.Identity) == 2; }));
        fixture.Orchestrator->Stop();
        VERIFY_IS_TRUE(fixture.Store->LoadTask(task.Task.Id)->Runs.front().State == TaskRunState::WaitingForDirectory);
        static_cast<void>(lease.Lease.Release());
        RuntimeTaskOrchestratorConfiguration configuration;
        configuration.ActivePollMilliseconds = 50;
        configuration.IdlePollMilliseconds = 100;
        fixture.Orchestrator = std::make_unique<RuntimeTaskOrchestrator>(
            fixture.Store, fixture.Provider, fixture.Rtask, fixture.Terminals, fixture.Root.Path(), std::filesystem::path{},
            LR"(C:\test\rtask.exe)", R"(\\.\pipe\orchestrator-restart)", configuration);
        VERIFY_IS_TRUE(fixture.Orchestrator->Start());
        VERIFY_IS_TRUE(_waitUntil([&] {
            const auto aggregate = fixture.Store->LoadTask(task.Task.Id);
            return aggregate->Runs.size() == 1 && aggregate->Runs.front().State == TaskRunState::Completed;
        }));
    }

    void RuntimeTaskOrchestratorTests::StopCloseAndDeleteWaitForProviderCompletionAndKeepDirectoryLease()
    {
        using namespace Microsoft::Terminal::TaskExecution;
        for (const auto kind : { TaskIntentKind::Stop, TaskIntentKind::Close, TaskIntentKind::Delete })
        {
            OrchestratorFixture fixture;
            fixture.State->CompleteOnStart = false;
            fixture.State->InterruptStatus = TaskProviderStatus::Rejected;
            const auto task = fixture.CreateDirectTask();
            VERIFY_IS_TRUE(fixture.Store->CreateTaskRun(MakeCreateRunCommand(task, 1, 2, 1, 1100)).Succeeded());
            const auto directory = ResolveCanonicalDirectoryIdentity(fixture.Root.Path(), DirectoryIdentityPolicy::Direct);
            VERIFY_IS_TRUE(directory.Succeeded());
            DirectDirectoryLeaseManager observer;
            VERIFY_IS_TRUE(fixture.Orchestrator->Start());
            VERIFY_IS_TRUE(_waitUntil([&] {
                return fixture.Store->LoadTask(task.Task.Id)->Runs.front().State == TaskRunState::Running;
            }));
            auto aggregate = fixture.Store->LoadTask(task.Task.Id);
            VERIFY_IS_TRUE(fixture.Store->StartTaskIntent(MakeStartIntentCommand(
                task, 1, 20, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds + 1, kind)).Succeeded());
            fixture.Orchestrator->Notify();
            VERIFY_IS_TRUE(_waitUntil([&] {
                const auto current = fixture.Store->LoadTask(task.Task.Id);
                return current && current->Intent && current->Intent->ReasonCode ==
                    (kind == TaskIntentKind::Stop ? "provider-interrupt-failed" : "provider-close-failed");
            }));
            aggregate = fixture.Store->LoadTask(task.Task.Id);
            VERIFY_IS_TRUE(aggregate.has_value());
            VERIFY_IS_TRUE(aggregate->Runs.front().State == TaskRunState::Running);
            VERIFY_ARE_EQUAL(size_t{ 1 }, observer.ParticipantCount(*directory.Identity));

            fixture.State->InterruptStatus = TaskProviderStatus::Succeeded;
            VERIFY_IS_TRUE(_waitUntil([&] {
                const auto current = fixture.Store->LoadTask(task.Task.Id);
                return current && current->Intent && current->Intent->ReasonCode ==
                    (kind == TaskIntentKind::Stop ? "provider-interrupt-pending" : "provider-close-pending");
            }));
            aggregate = fixture.Store->LoadTask(task.Task.Id);
            VERIFY_IS_TRUE(aggregate.has_value());
            VERIFY_IS_TRUE(aggregate->Runs.front().State == TaskRunState::Running);
            VERIFY_ARE_EQUAL(size_t{ 1 }, observer.ParticipantCount(*directory.Identity));

            fixture.State->TerminalEvent = TaskProviderEventKind::RunInterrupted;
            VERIFY_IS_TRUE(_waitUntil([&] {
                const auto current = fixture.Store->LoadTask(task.Task.Id);
                if (kind == TaskIntentKind::Delete)
                    return !current;
                return current && current->Intent->Stage == TaskIntentStage::Completed &&
                       current->Runs.front().State == TaskRunState::Interrupted;
            }));
            VERIFY_ARE_EQUAL(size_t{ 0 }, observer.ParticipantCount(*directory.Identity));
        }
    }

    void RuntimeTaskOrchestratorTests::DeleteCancelsAnActualDirectoryWait()
    {
        using namespace Microsoft::Terminal::TaskExecution;
        OrchestratorFixture fixture;
        const auto task = fixture.CreateDirectTask();
        const auto directory = ResolveCanonicalDirectoryIdentity(fixture.Root.Path(), DirectoryIdentityPolicy::Direct);
        VERIFY_IS_TRUE(directory.Succeeded());
        DirectDirectoryLeaseManager owner;
        wil::unique_handle cancellation{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        auto lease = owner.Acquire(*directory.Identity, cancellation.get());
        VERIFY_IS_TRUE(lease.Acquired());
        VERIFY_IS_TRUE(fixture.Store->CreateTaskRun(MakeCreateRunCommand(task, 1, 2, 1, 1100)).Succeeded());
        VERIFY_IS_TRUE(fixture.Orchestrator->Start());
        VERIFY_IS_TRUE(_waitUntil([&] {
            return fixture.Store->LoadTask(task.Task.Id)->Runs.front().State == TaskRunState::WaitingForDirectory &&
                   owner.ParticipantCount(*directory.Identity) == 2;
        }));
        const auto aggregate = fixture.Store->LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(fixture.Store->StartTaskIntent(MakeStartIntentCommand(
            task, 1, 20, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds + 1, TaskIntentKind::Delete)).Succeeded());
        fixture.Orchestrator->Notify();
        VERIFY_IS_TRUE(_waitUntil([&] { return !fixture.Store->LoadTask(task.Task.Id); }));
        VERIFY_IS_TRUE(static_cast<bool>(lease.Lease));
        VERIFY_ARE_EQUAL(size_t{ 1 }, owner.ParticipantCount(*directory.Identity));
    }

    void RuntimeTaskOrchestratorTests::WorktreeTaskCreatedThroughProjectionRunsToCompletion()
    {
        using namespace TaskGitLocalTests::TestSupport;
        GitFixture git;
        const auto repository = git.Temporary.Path() / L"SourceRepository";
        VERIFY_IS_TRUE(git.Initialize(repository));
        VERIFY_IS_TRUE(git.Run(repository, { L"config", L"user.name", L"Task Review Test" }).Succeeded());
        VERIFY_IS_TRUE(git.Run(repository, { L"config", L"user.email", L"task-test@example.invalid" }).Succeeded());
        VERIFY_IS_TRUE(git.Run(repository, { L"config", L"commit.gpgSign", L"false" }).Succeeded());
        OrchestratorFixture fixture{ git.GitExecutable };
        RuntimeTaskProjectionService projection{ fixture.Store, fixture.Provider, RequireIdentity(fixture.Root.Path()).SettingsRoot };
        Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest create;
        create.CommandId = MakeId<CommandId>(1).Value();
        create.TaskId = MakeId<TaskId>(1).Value();
        create.Operation = Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandOperation::Create;
        create.Draft.emplace();
        create.Draft->WorkspaceId = MakeId<WorkspaceId>(1).Value();
        create.Draft->Title = "Canonical Worktree run";
        create.Draft->SourceRoot = Utf8Path(repository);
        create.Draft->ExecutionMode = Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionExecutionMode::Worktree;
        VERIFY_IS_TRUE(projection.Execute(create, nullptr).Succeeded());
        auto start = create;
        start.CommandId = MakeId<CommandId>(2).Value();
        start.Operation = Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandOperation::Start;
        start.ExpectedTaskVersion = 1;
        start.Draft.reset();
        VERIFY_IS_TRUE(projection.Execute(start, nullptr).Succeeded());
        VERIFY_IS_TRUE(fixture.Orchestrator->Start());
        VERIFY_IS_TRUE(_waitUntil([&] {
            const auto aggregate = fixture.Store->LoadTask(TaskId{ create.TaskId });
            return aggregate && aggregate->Runs.front().State == TaskRunState::Completed &&
                   aggregate->GitWorktree && aggregate->GitWorktree->State == TaskGitWorktreeState::Ready &&
                   !aggregate->GitCheckpoints.empty() &&
                   (aggregate->GitCheckpoints.front().State == TaskGitCheckpointState::Succeeded ||
                    aggregate->GitCheckpoints.front().State == TaskGitCheckpointState::NotNeeded);
        }, 30'000));
    }

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

    void RuntimeTaskOrchestratorTests::ManualAndAutomaticRetriesRetainFollowupInput()
    {
        const auto task = MakeCreateTaskCommand();
        auto original = MakeCreateRunCommand(task, 1, 2, 1, 1100).Run;
        original.Trigger = TaskRunTrigger::FollowUp;
        original.State = TaskRunState::Failed;
        auto manual = original;
        manual.Id = MakeId<TaskRunId>(3);
        manual.Trigger = TaskRunTrigger::ManualRetry;
        manual.RetryOfRunId = original.Id;
        auto automatic = manual;
        automatic.Id = MakeId<TaskRunId>(4);
        automatic.Trigger = TaskRunTrigger::AutomaticRetry;
        automatic.RetryOfRunId = manual.Id;
        TaskAggregateSnapshot aggregate;
        aggregate.Task = task.Task;
        aggregate.Runs = { original, manual, automatic };
        aggregate.RunMessages = {
            { original.Id, MakeId<TaskMessageId>(1), 0, 2, "first canonical follow-up" },
            { original.Id, MakeId<TaskMessageId>(2), 1, 1, "second canonical follow-up" },
            { MakeId<TaskRunId>(99), MakeId<TaskMessageId>(3), 0, 1, "unrelated input" },
        };
        const auto prompt = BuildRuntimeTaskPrompt(aggregate, original);
        VERIFY_IS_TRUE(prompt.has_value());
        VERIFY_IS_TRUE(prompt->find("first canonical follow-up") != std::string::npos);
        VERIFY_IS_TRUE(prompt->find("unrelated input") == std::string::npos);
        VERIFY_IS_TRUE(BuildRuntimeTaskPrompt(aggregate, manual) == prompt);
        VERIFY_IS_TRUE(BuildRuntimeTaskPrompt(aggregate, automatic) == prompt);
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

    void RuntimeTaskOrchestratorTests::IdleTerminalWithoutProviderSessionSuspends()
    {
        TemporaryDatabasePath database;
        UniqueRuntimeRoot runtimeRoot;
        std::filesystem::create_directory(runtimeRoot.Path());
        const auto store = std::make_shared<SqliteTaskStore>(database.Path());
        const auto task = MakeCreateTaskCommand();
        VERIFY_IS_TRUE(store->CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store->ChangeTaskTerminalState(
                                MakeTerminalStateCommand(
                                    task, 2, 1, 1100,
                                    TaskTerminalState::Starting, 1, true))
                           .Succeeded());
        VERIFY_IS_TRUE(store->ChangeTaskTerminalState(
                                MakeTerminalStateCommand(
                                    task, 3, 2, 1200,
                                    TaskTerminalState::Ready, 1))
                           .Succeeded());

        const auto identity = RequireIdentity(runtimeRoot.Path());
        auto terminals = std::make_shared<RuntimeTaskTerminalService>(
            identity,
            "b3000000-0000-4000-8000-000000000003",
            [](auto&&, auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; },
            [](auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; });
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
        configuration.TerminalIdleMilliseconds = 1000;
        RuntimeTaskOrchestrator orchestrator{
            store, provider, rtask, terminals, runtimeRoot.Path(), {},
            LR"(C:\test\rtask.exe)", R"(\\.\pipe\orchestrator-idle-test)", configuration
        };
        VERIFY_IS_TRUE(orchestrator.Start());
        orchestrator.Notify();
        VERIFY_IS_TRUE(_waitUntil([&] {
            const auto aggregate = store->LoadTask(task.Task.Id);
            return aggregate && aggregate->Terminal.State == TaskTerminalState::Suspended;
        }, 5000));
        orchestrator.Stop();
        provider->Stop();
        terminals->Stop();
    }
}
