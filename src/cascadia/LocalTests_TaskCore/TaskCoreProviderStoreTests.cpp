// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskCoreStoreTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    namespace
    {
        RecordTaskAgentSessionCommand _session(
            const CreateTaskCommand& task,
            const uint64_t commandSeed,
            const uint64_t version,
            const int64_t timestamp,
            const AgentSessionState state = AgentSessionState::Available,
            const bool current = true)
        {
            RecordTaskAgentSessionCommand command;
            command.Metadata = MakeMetadata(commandSeed, version, timestamp);
            command.Session.Id = MakeId<AgentSessionId>(7000 + task.Task.Version);
            command.Session.Task = task.Task.Id;
            command.Session.Provider = "codex";
            command.Session.ProviderSessionId = "provider-thread-1";
            command.Session.State = state;
            command.Session.IsCurrent = current;
            command.Session.CreatedAtUnixMilliseconds = 1100;
            command.Session.UpdatedAtUnixMilliseconds = timestamp;
            return command;
        }

        AppendTaskProviderEventCommand _event(
            const CreateTaskCommand& task,
            const TaskRunId& run,
            const uint64_t commandSeed,
            const uint64_t version,
            const int64_t timestamp)
        {
            AppendTaskProviderEventCommand command;
            command.Metadata = MakeMetadata(commandSeed, version, timestamp);
            command.Metadata.EventPayloadJson = R"({"providerSequence":7,"kind":"agent_message_delta","text":"hello"})";
            command.Task = task.Task.Id;
            command.Run = run;
            command.ProviderSequence = 7;
            command.EventType = "ProviderAgentMessageDelta";
            return command;
        }
    }

    class TaskCoreProviderStoreTests
    {
        TEST_CLASS(TaskCoreProviderStoreTests);

        TEST_METHOD(RecordsAndTransitionsCurrentAgentSessionAtomically);
        TEST_METHOD(AppendsOrderedProviderEventAndReplaysReceipt);
        TEST_METHOD(TelemetryDoesNotInvalidateBusinessCommandsOrDependOnTheirRevision);
        TEST_METHOD(RejectsCrossTaskBindingsAndRollsBackEventFailure);
        TEST_METHOD(RunningBindsSessionAtomicallyAndPreservesHistory);
        TEST_METHOD(RunSessionBindingRejectsForeignAndReplacementSessions);
    };

    void TaskCoreProviderStoreTests::RunningBindsSessionAtomicallyAndPreservesHistory()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        const auto session = _session(task, 3, 2, 1200);
        {
            SqliteTaskStore store{ path.Path() };
            VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
            VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
            VERIFY_IS_TRUE(store.RecordTaskAgentSession(session).Succeeded());
            VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(run, 4, 3, 1300, TaskRunState::Preparing)).Succeeded());
            VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(run, 5, 4, 1400, TaskRunState::Starting)).Succeeded());
            auto running = MakeRunStateCommand(run, 6, 5, 1500, TaskRunState::Running);
            running.AgentSession = session.Session.Id;

            auto database = OpenRawDatabase(path.Path());
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(database,
                "CREATE TRIGGER fail_run_binding BEFORE INSERT ON task_event "
                "WHEN NEW.type='TaskRunStateChanged' BEGIN SELECT RAISE(ABORT, 'binding event failure'); END;",
                nullptr, nullptr, nullptr));
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
            VERIFY_IS_TRUE(store.ChangeTaskRunState(running).Status == TaskStoreCommandStatus::StorageError);
            const auto unchanged = store.LoadTask(task.Task.Id);
            VERIFY_ARE_EQUAL(uint64_t{ 5 }, unchanged->Task.Version);
            VERIFY_IS_TRUE(unchanged->Runs.front().State == TaskRunState::Starting);
            VERIFY_IS_FALSE(unchanged->Runs.front().AgentSession.has_value());
            VERIFY_IS_FALSE(unchanged->Runs.front().StartedAtUnixMilliseconds.has_value());

            database = OpenRawDatabase(path.Path());
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(database, "DROP TRIGGER fail_run_binding;", nullptr, nullptr, nullptr));
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
            VERIFY_IS_TRUE(store.ChangeTaskRunState(running).Succeeded());
            VERIFY_IS_TRUE(store.ChangeTaskRunState(running).Status == TaskStoreCommandStatus::Replayed);
            VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(run, 7, 6, 1600, TaskRunState::Completed)).Succeeded());
        }
        SqliteTaskStore reopened{ path.Path() };
        const auto aggregate = reopened.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(aggregate->Runs.front().AgentSession == std::optional{ session.Session.Id });
        VERIFY_IS_TRUE(aggregate->Runs.front().State == TaskRunState::Completed);
        VERIFY_IS_TRUE(aggregate->Runs.front().StartedAtUnixMilliseconds == std::optional<int64_t>{ 1500 });
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, aggregate->LastEventSequence);
    }

    void TaskCoreProviderStoreTests::RunSessionBindingRejectsForeignAndReplacementSessions()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        const auto first = MakeCreateTaskCommand();
        const auto second = MakeCreateTaskCommand(2, 1, 2, 1000);
        VERIFY_IS_TRUE(store.CreateTask(first).Succeeded());
        VERIFY_IS_TRUE(store.CreateTask(second).Succeeded());
        const auto run = MakeCreateRunCommand(first, 1, 3, 1, 1100);
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        const auto owned = _session(first, 4, 2, 1200);
        VERIFY_IS_TRUE(store.RecordTaskAgentSession(owned).Succeeded());
        auto foreign = _session(second, 5, 1, 1200);
        foreign.Session.Id = MakeId<AgentSessionId>(7998);
        foreign.Session.ProviderSessionId = "foreign-thread";
        VERIFY_IS_TRUE(store.RecordTaskAgentSession(foreign).Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(run, 6, 3, 1300, TaskRunState::Preparing)).Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(run, 7, 4, 1400, TaskRunState::Starting)).Succeeded());
        auto running = MakeRunStateCommand(run, 8, 5, 1500, TaskRunState::Running);
        running.AgentSession = AgentSessionId{ "invalid" };
        VERIFY_IS_TRUE(store.ChangeTaskRunState(running).Status == TaskStoreCommandStatus::InvalidCommand);
        running.AgentSession = MakeId<AgentSessionId>(7997);
        VERIFY_IS_TRUE(store.ChangeTaskRunState(running).Status == TaskStoreCommandStatus::InvalidRecord);
        running.AgentSession = foreign.Session.Id;
        VERIFY_IS_TRUE(store.ChangeTaskRunState(running).Status == TaskStoreCommandStatus::InvalidRecord);
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, store.LoadTask(first.Task.Id)->Task.Version);
        running.AgentSession = owned.Session.Id;
        VERIFY_IS_TRUE(store.ChangeTaskRunState(running).Succeeded());

        auto replacement = _session(first, 9, 6, 1600);
        replacement.Session.Id = MakeId<AgentSessionId>(7999);
        replacement.Session.ProviderSessionId = "replacement-thread";
        VERIFY_IS_TRUE(store.RecordTaskAgentSession(replacement).Succeeded());
        auto rebind = MakeRunStateCommand(run, 10, 7, 1700, TaskRunState::Running);
        rebind.AgentSession = replacement.Session.Id;
        VERIFY_IS_TRUE(store.ChangeTaskRunState(rebind).Status == TaskStoreCommandStatus::InvalidTransition);
        rebind.RequestedState = TaskRunState::Completed;
        VERIFY_IS_TRUE(store.ChangeTaskRunState(rebind).Status == TaskStoreCommandStatus::InvalidCommand);
        const auto aggregate = store.LoadTask(first.Task.Id);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, aggregate->Task.Version);
        VERIFY_IS_TRUE(aggregate->Runs.front().State == TaskRunState::Running);
        VERIFY_IS_TRUE(aggregate->Runs.front().AgentSession == std::optional{ owned.Session.Id });
    }

    void TaskCoreProviderStoreTests::RecordsAndTransitionsCurrentAgentSessionAtomically()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());

        auto recorded = _session(task, 2, 1, 1100);
        const auto result = store.RecordTaskAgentSession(recorded);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, result.CommittedTaskVersion);
        auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->AgentSessions.size());
        VERIFY_IS_TRUE(snapshot->AgentSessions.front().IsCurrent);
        VERIFY_IS_TRUE(snapshot->Terminal.CurrentAgentSessionId == std::optional{ recorded.Session.Id });

        VERIFY_IS_TRUE(store.RecordTaskAgentSession(recorded).Status == TaskStoreCommandStatus::Replayed);
        recorded.Metadata = MakeMetadata(3, 2, 1200);
        recorded.Session.State = AgentSessionState::Suspended;
        recorded.Session.IsCurrent = false;
        recorded.Session.UpdatedAtUnixMilliseconds = 1200;
        VERIFY_IS_TRUE(store.RecordTaskAgentSession(recorded).Succeeded());
        snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot->AgentSessions.front().State == AgentSessionState::Suspended);
        VERIFY_IS_FALSE(snapshot->AgentSessions.front().IsCurrent);
        VERIFY_IS_FALSE(snapshot->Terminal.CurrentAgentSessionId.has_value());

        recorded.Metadata = MakeMetadata(4, 3, 1300);
        recorded.Session.State = AgentSessionState::Available;
        recorded.Session.IsCurrent = true;
        recorded.Session.UpdatedAtUnixMilliseconds = 1300;
        VERIFY_IS_TRUE(store.RecordTaskAgentSession(recorded).Succeeded());
    }

    void TaskCoreProviderStoreTests::AppendsOrderedProviderEventAndReplaysReceipt()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        const auto recorded = _session(task, 2, 1, 1100);
        VERIFY_IS_TRUE(store.RecordTaskAgentSession(recorded).Succeeded());
        auto run = MakeCreateRunCommand(task, 1, 3, 2, 1200);
        run.Run.AgentSession = recorded.Session.Id;
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());

        const auto command = _event(task, run.Run.Id, 4, 3, 1300);
        const auto appended = store.AppendTaskProviderEvent(command);
        VERIFY_IS_TRUE(appended.Succeeded());
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, appended.CommittedTaskVersion);
        VERIFY_IS_TRUE(store.AppendTaskProviderEvent(command).Status == TaskStoreCommandStatus::Replayed);

        const auto events = store.ListEvents(task.Task.Id, 0, 16);
        VERIFY_ARE_EQUAL(size_t{ 4 }, events.size());
        VERIFY_ARE_EQUAL(std::string{ "ProviderAgentMessageDelta" }, events.back().Type);
        VERIFY_IS_TRUE(events.back().RunId == std::optional{ run.Run.Id });
        VERIFY_IS_TRUE(events.back().RunSequence == std::optional<uint64_t>{ 2 });
        VERIFY_IS_TRUE(events.back().PayloadJson.find("providerSequence") != std::string::npos);
    }

    void TaskCoreProviderStoreTests::TelemetryDoesNotInvalidateBusinessCommandsOrDependOnTheirRevision()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        VERIFY_IS_TRUE(store.AppendTaskProviderEvent(_event(task, run.Run.Id, 3, 2, 1200)).Succeeded());
        auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(int64_t{ 1100 }, snapshot->Task.UpdatedAtUnixMilliseconds);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, snapshot->LastEventSequence);
        VERIFY_IS_TRUE(store.ChangeTaskManualHold(MakeManualHoldCommand(task, 4, 2, 1400, true)).Succeeded());
        auto delayed = _event(task, run.Run.Id, 5, 2, 1300);
        delayed.ProviderSequence = 8;
        delayed.Metadata.EventPayloadJson = R"({"providerSequence":8,"kind":"agent_message_delta","text":"later"})";
        VERIFY_IS_TRUE(store.AppendTaskProviderEvent(delayed).Succeeded());
        VERIFY_IS_TRUE(store.AppendTaskProviderEvent(delayed).Status == TaskStoreCommandStatus::Replayed);
        snapshot = store.LoadTask(task.Task.Id);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(int64_t{ 1400 }, snapshot->Task.UpdatedAtUnixMilliseconds);
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, snapshot->LastEventSequence);
        VERIFY_IS_TRUE(store.ChangeTaskManualHold(MakeManualHoldCommand(task, 6, 2, 1600, false)).Status == TaskStoreCommandStatus::VersionConflict);
    }

    void TaskCoreProviderStoreTests::RejectsCrossTaskBindingsAndRollsBackEventFailure()
    {
        TemporaryDatabasePath path;
        const auto first = MakeCreateTaskCommand(1, 1, 1, 1000);
        const auto second = MakeCreateTaskCommand(2, 1, 2, 1000);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(first).Succeeded());
        VERIFY_IS_TRUE(store.CreateTask(second).Succeeded());
        const auto recorded = _session(first, 3, 1, 1100);
        VERIFY_IS_TRUE(store.RecordTaskAgentSession(recorded).Succeeded());

        auto collision = _session(second, 4, 1, 1100);
        collision.Session.Id = MakeId<AgentSessionId>(7999);
        VERIFY_IS_TRUE(store.RecordTaskAgentSession(collision).Status == TaskStoreCommandStatus::StorageError);

        auto run = MakeCreateRunCommand(first, 1, 5, 2, 1200);
        run.Run.AgentSession = recorded.Session.Id;
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        sqlite3* database = OpenRawDatabase(path.Path());
        constexpr auto trigger = R"SQL(
CREATE TRIGGER fail_provider_event BEFORE INSERT ON task_event
WHEN NEW.type='ProviderAgentMessageDelta'
BEGIN SELECT RAISE(ABORT, 'provider event failure'); END;
)SQL";
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(database, trigger, nullptr, nullptr, nullptr));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));

        const auto failed = store.AppendTaskProviderEvent(_event(first, run.Run.Id, 6, 3, 1300));
        VERIFY_IS_TRUE(failed.Status == TaskStoreCommandStatus::StorageError);
        const auto snapshot = store.LoadTask(first.Task.Id);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, snapshot->LastEventSequence);
    }
}
