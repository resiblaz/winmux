// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeTaskProviderSink.h"
#include "RuntimeTaskSchedulingPolicy.h"
#include "TaskCoreStoreTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskProvider;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        TaskProviderSessionBinding _session(const CreateTaskCommand& task)
        {
            return { task.Task.Id.Value(), "provider-thread-1", TaskProviderSessionState::Ready };
        }

        TaskProviderRunBinding _binding(
            const CreateTaskCommand& task,
            const CreateTaskRunCommand& run,
            const TaskProviderRunState state)
        {
            return {
                task.Task.Id.Value(),
                run.Run.Id.Value(),
                "provider-thread-1",
                "provider-turn-1",
                state,
            };
        }
    }

    class RuntimeTaskProviderSinkTests
    {
        TEST_CLASS(RuntimeTaskProviderSinkTests);

        TEST_METHOD(PersistsSessionRunEventsFailureAndDeferredSteering);
        TEST_METHOD(BatchesSequentialProviderEventsWithoutLosingOrder);
        TEST_METHOD(RejectsUnknownAndCrossTaskProviderIdentity);
        TEST_METHOD(RejectsUnownedSessionAndSessionReplacement);
        TEST_METHOD(BindsAnUnboundRunningRunWithoutResettingItsStartTime);
    };

    void RuntimeTaskProviderSinkTests::PersistsSessionRunEventsFailureAndDeferredSteering()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        VERIFY_IS_TRUE(store->CreateTask(task).Succeeded());
        RuntimeTaskProviderSink sink{ store };
        VERIFY_IS_TRUE(sink.Valid());

        const auto run = MakeCreateRunCommand(task, 1, 20, 1, 1100);
        VERIFY_IS_TRUE(store->CreateTaskRun(run).Succeeded());
        auto session = _session(task);
        VERIFY_IS_TRUE(sink.OnSessionStateChanged(session).Succeeded());
        auto aggregate = store->LoadTask(task.Task.Id);
        VERIFY_ARE_EQUAL(size_t{ 1 }, aggregate->AgentSessions.size());
        VERIFY_IS_TRUE(aggregate->AgentSessions.front().IsCurrent);

        VERIFY_IS_FALSE(aggregate->Runs.front().AgentSession.has_value());
        VERIFY_IS_TRUE(sink.OnRunStateChanged(_binding(task, run, TaskProviderRunState::Running), std::nullopt).Succeeded());
        aggregate = store->LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(aggregate->Runs.front().AgentSession == std::optional{ aggregate->AgentSessions.front().Id });
        const auto boundVersion = aggregate->Task.Version;
        VERIFY_IS_TRUE(sink.OnRunStateChanged(_binding(task, run, TaskProviderRunState::Running), std::nullopt).Status == TaskProviderStatus::AlreadyApplied);
        VERIFY_ARE_EQUAL(boundVersion, store->LoadTask(task.Task.Id)->Task.Version);

        TaskProviderEvent event;
        event.Sequence = 1;
        event.Kind = TaskProviderEventKind::AgentMessageDelta;
        event.SessionId = "provider-thread-1";
        event.RunId = "provider-turn-1";
        event.ItemId = "item-1";
        event.Text = "hello from provider";
        event.PayloadJson = R"({"delta":"hello from provider"})";
        VERIFY_IS_TRUE(sink.OnEvent(session, _binding(task, run, TaskProviderRunState::Running), event).Succeeded());
        VERIFY_IS_TRUE(sink.OnSteeringDeferred(
            _binding(task, run, TaskProviderRunState::Running),
            "follow up after current turn", "provider_steering_rejected").Succeeded());

        TaskProviderFailureDisposition failure{
            TaskProviderFailureClass::RuntimeRecovery,
            TaskProviderResumeSafety::Safe,
            true,
            2,
            "runtime_recovery",
        };
        VERIFY_IS_TRUE(sink.OnRunStateChanged(
            _binding(task, run, TaskProviderRunState::Interrupted), failure).Succeeded());
        session.State = TaskProviderSessionState::Suspended;
        VERIFY_IS_TRUE(sink.OnSessionStateChanged(session).Succeeded());

        aggregate = store->LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(aggregate->Runs.front().State == TaskRunState::Interrupted);
        VERIFY_ARE_EQUAL(std::string{ "runtime_recovery" }, aggregate->Runs.front().ReasonCode);
        VERIFY_IS_TRUE(aggregate->AgentSessions.front().State == AgentSessionState::Suspended);
        VERIFY_IS_TRUE(aggregate->Runs.front().AgentSession == std::optional{ aggregate->AgentSessions.front().Id });
        auto nextRun = run.Run;
        nextRun.Id = MakeId<TaskRunId>(9000);
        nextRun.Sequence = 2;
        VERIFY_IS_TRUE(CanResumeRuntimeTaskAgentSession(*aggregate, nextRun, aggregate->AgentSessions.front()));
        VERIFY_ARE_EQUAL(size_t{ 1 }, aggregate->Messages.size());
        VERIFY_IS_TRUE(aggregate->Messages.front().State == TaskMessageState::Deferred);
        const auto events = store->ListEvents(task.Task.Id, 0, 32);
        VERIFY_IS_TRUE(std::any_of(events.begin(), events.end(), [](const auto& value) {
            return value.Type == "Provider_agent_message_delta" &&
                   value.PayloadJson.find("hello from provider") != std::string::npos;
        }));
    }

    void RuntimeTaskProviderSinkTests::RejectsUnownedSessionAndSessionReplacement()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto other = MakeCreateTaskCommand(2, 1, 2, 1000);
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        VERIFY_IS_TRUE(store->CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store->CreateTask(other).Succeeded());
        const auto run = MakeCreateRunCommand(task, 1, 3, 1, 1100);
        VERIFY_IS_TRUE(store->CreateTaskRun(run).Succeeded());
        RuntimeTaskProviderSink sink{ store };
        auto binding = _binding(task, run, TaskProviderRunState::Running);
        VERIFY_IS_TRUE(sink.OnRunStateChanged(binding, std::nullopt).Status == TaskProviderStatus::InvalidRequest);
        auto foreign = _session(other);
        foreign.ProviderSessionId = "foreign-thread";
        VERIFY_IS_TRUE(sink.OnSessionStateChanged(foreign).Succeeded());
        binding.ProviderSessionId = foreign.ProviderSessionId;
        VERIFY_IS_TRUE(sink.OnRunStateChanged(binding, std::nullopt).Status == TaskProviderStatus::InvalidRequest);
        VERIFY_IS_TRUE(store->LoadTask(task.Task.Id)->Runs.front().State == TaskRunState::Queued);

        VERIFY_IS_TRUE(sink.OnSessionStateChanged(_session(task)).Succeeded());
        binding = _binding(task, run, TaskProviderRunState::Running);
        VERIFY_IS_TRUE(sink.OnRunStateChanged(binding, std::nullopt).Succeeded());
        const auto original = store->LoadTask(task.Task.Id)->Runs.front().AgentSession;
        auto replacement = _session(task);
        replacement.ProviderSessionId = "replacement-thread";
        VERIFY_IS_TRUE(sink.OnSessionStateChanged(replacement).Succeeded());
        binding.ProviderSessionId = replacement.ProviderSessionId;
        VERIFY_IS_TRUE(sink.OnRunStateChanged(binding, std::nullopt).Status == TaskProviderStatus::InvalidRequest);
        binding.State = TaskProviderRunState::Completed;
        VERIFY_IS_TRUE(sink.OnRunStateChanged(binding, std::nullopt).Status == TaskProviderStatus::InvalidRequest);
        const auto aggregate = store->LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(aggregate->Runs.front().AgentSession == original);
        VERIFY_IS_TRUE(aggregate->Runs.front().State == TaskRunState::Running);
    }

    void RuntimeTaskProviderSinkTests::BindsAnUnboundRunningRunWithoutResettingItsStartTime()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        VERIFY_IS_TRUE(store->CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store->CreateTaskRun(run).Succeeded());
        VERIFY_IS_TRUE(store->ChangeTaskRunState(MakeRunStateCommand(run, 3, 2, 1200, TaskRunState::Preparing)).Succeeded());
        VERIFY_IS_TRUE(store->ChangeTaskRunState(MakeRunStateCommand(run, 4, 3, 1300, TaskRunState::Starting)).Succeeded());
        VERIFY_IS_TRUE(store->ChangeTaskRunState(MakeRunStateCommand(run, 5, 4, 1400, TaskRunState::Running)).Succeeded());
        RuntimeTaskProviderSink sink{ store };
        VERIFY_IS_TRUE(sink.OnSessionStateChanged(_session(task)).Succeeded());
        VERIFY_IS_TRUE(sink.OnRunStateChanged(_binding(task, run, TaskProviderRunState::Running), std::nullopt).Succeeded());
        const auto aggregate = store->LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(aggregate->Runs.front().AgentSession == std::optional{ aggregate->AgentSessions.front().Id });
        VERIFY_IS_TRUE(aggregate->Runs.front().StartedAtUnixMilliseconds == std::optional<int64_t>{ 1400 });
    }

    void RuntimeTaskProviderSinkTests::BatchesSequentialProviderEventsWithoutLosingOrder()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        VERIFY_IS_TRUE(store->CreateTask(task).Succeeded());
        RuntimeTaskProviderSink sink{ store };
        auto session = _session(task);
        VERIFY_IS_TRUE(sink.OnSessionStateChanged(session).Succeeded());
        auto aggregate = store->LoadTask(task.Task.Id);
        auto run = MakeCreateRunCommand(task, 1, 20, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds + 1);
        run.Run.AgentSession = aggregate->AgentSessions.front().Id;
        VERIFY_IS_TRUE(store->CreateTaskRun(run).Succeeded());
        VERIFY_IS_TRUE(sink.OnRunStateChanged(_binding(task, run, TaskProviderRunState::Running), std::nullopt).Succeeded());

        for (uint64_t sequence = 1; sequence <= 100; ++sequence)
        {
            TaskProviderEvent event;
            event.Sequence = sequence;
            event.Kind = TaskProviderEventKind::ToolProgress;
            event.SessionId = "provider-thread-1";
            event.RunId = "provider-turn-1";
            event.ItemId = "tool-1";
            event.Text = "progress";
            VERIFY_IS_TRUE(sink.OnEvent(session, _binding(task, run, TaskProviderRunState::Running), event).Succeeded());
        }
        const auto events = store->ListEvents(task.Task.Id, 0, 256);
        const auto providerCount = std::count_if(events.begin(), events.end(), [](const auto& value) {
            return value.Type == "Provider_tool_progress";
        });
        VERIFY_ARE_EQUAL(size_t{ 100 }, static_cast<size_t>(providerCount));
        for (size_t index = 1; index < events.size(); ++index)
        {
            VERIFY_ARE_EQUAL(events[index - 1].AggregateSequence + 1, events[index].AggregateSequence);
        }
    }

    void RuntimeTaskProviderSinkTests::RejectsUnknownAndCrossTaskProviderIdentity()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        VERIFY_IS_TRUE(store->CreateTask(task).Succeeded());
        RuntimeTaskProviderSink sink{ store };
        TaskProviderSessionBinding invalid{ "not-a-task", "provider-thread", TaskProviderSessionState::Ready };
        VERIFY_IS_TRUE(sink.OnSessionStateChanged(invalid).Status == TaskProviderStatus::InvalidRequest);

        TaskProviderEvent event;
        event.Sequence = 1;
        event.Kind = TaskProviderEventKind::UnknownOptional;
        event.SessionId = "provider-thread";
        VERIFY_IS_TRUE(sink.OnEvent(invalid, std::nullopt, event).Status == TaskProviderStatus::InvalidRequest);

        invalid.TaskId = MakeId<TaskId>(9999).Value();
        VERIFY_IS_TRUE(sink.OnSessionStateChanged(invalid).Status == TaskProviderStatus::NotFound);
    }
}
