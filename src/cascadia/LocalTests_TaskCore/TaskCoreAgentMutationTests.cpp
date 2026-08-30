// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskCoreStoreTestFixtures.h"
#include "TaskStoreCommandValidation.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    namespace
    {
        uint64_t _startRunning(
            SqliteTaskStore& store,
            const CreateTaskRunCommand& run,
            const uint64_t startingVersion = 2,
            const uint64_t commandSeed = 20,
            const int64_t timestamp = 1200)
        {
            auto version = startingVersion;
            VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                    MakeRunStateCommand(
                                        run,
                                        commandSeed,
                                        version,
                                        timestamp,
                                        TaskRunState::Preparing))
                               .Succeeded());
            ++version;
            VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                    MakeRunStateCommand(
                                        run,
                                        commandSeed + 1,
                                        version,
                                        timestamp + 100,
                                        TaskRunState::Starting))
                               .Succeeded());
            ++version;
            VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                    MakeRunStateCommand(
                                        run,
                                        commandSeed + 2,
                                        version,
                                        timestamp + 200,
                                        TaskRunState::Running))
                               .Succeeded());
            return version + 1;
        }

        int64_t _receiptCount(sqlite3* database, const CommandId& command)
        {
            constexpr auto sql = "SELECT count(*) FROM command_receipt WHERE id=?1;";
            sqlite3_stmt* statement{};
            VERIFY_ARE_EQUAL(
                SQLITE_OK,
                sqlite3_prepare_v2(database, sql, -1, &statement, nullptr));
            VERIFY_ARE_EQUAL(
                SQLITE_OK,
                sqlite3_bind_text(
                    statement,
                    1,
                    command.Value().c_str(),
                    -1,
                    SQLITE_TRANSIENT));
            VERIFY_ARE_EQUAL(SQLITE_ROW, sqlite3_step(statement));
            const auto count = sqlite3_column_int64(statement, 0);
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_finalize(statement));
            return count;
        }
    }

    class TaskCoreAgentMutationTests
    {
        TEST_CLASS(TaskCoreAgentMutationTests);

        TEST_METHOD(FourAgentMutationsCommitStatusResultAndEvents);
        TEST_METHOD(ReplayedCommandIgnoresServerSelectedExpectedVersion);
        TEST_METHOD(CrossTaskAndInactiveRunMutationsFailWithoutReceipt);
        TEST_METHOD(IncompleteIntentBlocksAgentMutation);
        TEST_METHOD(AgentResultSurvivesRunCompletionWithoutReplacement);
        TEST_METHOD(FailedResultEventRollsBackMutationVersionEventAndReceipt);
        TEST_METHOD(ValidationRejectsTextShapeAndBounds);
    };

    void TaskCoreAgentMutationTests::FourAgentMutationsCommitStatusResultAndEvents()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        auto version = _startRunning(store, run);
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, version);

        const std::array statusKinds{
            TaskAgentMutationKind::StatusInProgress,
            TaskAgentMutationKind::StatusInReview,
            TaskAgentMutationKind::StatusInProgress,
            TaskAgentMutationKind::StatusBlocked,
            TaskAgentMutationKind::StatusInProgress,
        };
        for (size_t index = 0; index < statusKinds.size(); ++index)
        {
            const auto kind = statusKinds[index];
            auto command = MakeAgentMutationCommand(
                run,
                30 + index,
                version,
                1500 + static_cast<int64_t>(index) * 100,
                kind,
                kind == TaskAgentMutationKind::StatusBlocked ?
                    "waiting for user credentials" :
                    "");
            if (kind == TaskAgentMutationKind::StatusBlocked)
            {
                command.Metadata.EventPayloadJson =
                    R"({"method":"status_blocked","reason":"waiting for user credentials"})";
            }
            const auto applied = store.ApplyTaskAgentMutation(command);
            VERIFY_IS_TRUE(applied.Status == TaskStoreCommandStatus::Succeeded);
            ++version;
            VERIFY_ARE_EQUAL(version, applied.CommittedTaskVersion);
        }

        auto resultCommand = MakeAgentMutationCommand(
            run,
            40,
            version,
            2100,
            TaskAgentMutationKind::Result,
            "durable agent result");
        resultCommand.Metadata.EventPayloadJson =
            R"({"method":"result","textBytes":20})";
        const auto result = store.ApplyTaskAgentMutation(resultCommand);
        VERIFY_IS_TRUE(result.Status == TaskStoreCommandStatus::Succeeded);
        ++version;
        VERIFY_ARE_EQUAL(version, result.CommittedTaskVersion);

        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        if (!snapshot)
        {
            return;
        }
        VERIFY_IS_TRUE(snapshot->Task.Status == TaskStatus::InProgress);
        VERIFY_ARE_EQUAL(version, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(std::string{ "durable agent result" }, snapshot->Runs[0].Result);
        const auto events = store.ListEvents(task.Task.Id, 0, 32);
        VERIFY_ARE_EQUAL(size_t{ 11 }, events.size());
        VERIFY_ARE_EQUAL(std::string{ "AgentTaskStatusChanged" }, events[9].Type);
        VERIFY_ARE_EQUAL(std::string{ "TaskRunResultUpdated" }, events[10].Type);
        VERIFY_IS_TRUE(events[10].RunId == std::optional<TaskRunId>{ run.Run.Id });
        VERIFY_IS_TRUE(events[10].RunSequence == std::optional<uint64_t>{ 10 });
    }

    void TaskCoreAgentMutationTests::ReplayedCommandIgnoresServerSelectedExpectedVersion()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        const auto version = _startRunning(store, run);

        const auto command = MakeAgentMutationCommand(
            run,
            30,
            version,
            1500,
            TaskAgentMutationKind::StatusInProgress);
        const auto applied = store.ApplyTaskAgentMutation(command);
        VERIFY_IS_TRUE(applied.Status == TaskStoreCommandStatus::Succeeded);

        auto replay = command;
        replay.Metadata.ExpectedTaskVersion = applied.CommittedTaskVersion;
        replay.Metadata.TimestampUnixMilliseconds = 1600;
        const auto replayed = store.ApplyTaskAgentMutation(replay);
        VERIFY_IS_TRUE(replayed.Status == TaskStoreCommandStatus::Replayed);
        VERIFY_ARE_EQUAL(applied.CommittedTaskVersion, replayed.CommittedTaskVersion);
        VERIFY_ARE_EQUAL(applied.LastEventSequence, replayed.LastEventSequence);

        replay.Metadata.RequestDigest = std::string(64, 'f');
        VERIFY_IS_TRUE(
            store.ApplyTaskAgentMutation(replay).Status ==
            TaskStoreCommandStatus::IdempotencyConflict);
        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(applied.CommittedTaskVersion, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(applied.LastEventSequence, snapshot->LastEventSequence);
    }

    void TaskCoreAgentMutationTests::CrossTaskAndInactiveRunMutationsFailWithoutReceipt()
    {
        TemporaryDatabasePath path;
        const auto firstTask = MakeCreateTaskCommand(1, 1, 1, 1000);
        const auto secondTask = MakeCreateTaskCommand(2, 2, 2, 1000);
        const auto run = MakeCreateRunCommand(firstTask, 1, 3, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(firstTask).Succeeded());
        VERIFY_IS_TRUE(store.CreateTask(secondTask).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());

        auto queued = MakeAgentMutationCommand(
            run,
            30,
            2,
            1200,
            TaskAgentMutationKind::Result,
            "too early");
        VERIFY_IS_TRUE(
            store.ApplyTaskAgentMutation(queued).Status ==
            TaskStoreCommandStatus::InvalidTransition);

        const auto version = _startRunning(store, run);
        auto crossTask = MakeAgentMutationCommand(
            run,
            31,
            1,
            1500,
            TaskAgentMutationKind::Result,
            "wrong task");
        crossTask.Task = secondTask.Task.Id;
        VERIFY_IS_TRUE(
            store.ApplyTaskAgentMutation(crossTask).Status ==
            TaskStoreCommandStatus::NotFound);

        crossTask.Task = firstTask.Task.Id;
        crossTask.Metadata.ExpectedTaskVersion = version;
        const auto corrected = store.ApplyTaskAgentMutation(crossTask);
        VERIFY_IS_TRUE(corrected.Status == TaskStoreCommandStatus::Succeeded);
        const auto snapshot = store.LoadTask(firstTask.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(std::string{ "wrong task" }, snapshot->Runs[0].Result);
    }

    void TaskCoreAgentMutationTests::IncompleteIntentBlocksAgentMutation()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        const auto version = _startRunning(store, run);
        const auto intent = MakeStartIntentCommand(
            task,
            1,
            30,
            version,
            1500,
            TaskIntentKind::Stop);
        VERIFY_IS_TRUE(store.StartTaskIntent(intent).Succeeded());

        const auto mutation = MakeAgentMutationCommand(
            run,
            31,
            version + 1,
            1600,
            TaskAgentMutationKind::StatusInProgress);
        const auto blocked = store.ApplyTaskAgentMutation(mutation);
        VERIFY_IS_TRUE(blocked.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(blocked.Transition == TransitionError::ActiveIntentConflict);
    }

    void TaskCoreAgentMutationTests::AgentResultSurvivesRunCompletionWithoutReplacement()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        auto version = _startRunning(store, run);
        VERIFY_IS_TRUE(store.ApplyTaskAgentMutation(MakeAgentMutationCommand(
                                                         run,
                                                         30,
                                                         version,
                                                         1500,
                                                         TaskAgentMutationKind::Result,
                                                         "preserved result"))
                           .Succeeded());
        ++version;
        VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(
                                                    run,
                                                    31,
                                                    version,
                                                    1600,
                                                    TaskRunState::Completed))
                           .Succeeded());
        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Runs[0].State == TaskRunState::Completed);
        VERIFY_ARE_EQUAL(std::string{ "preserved result" }, snapshot->Runs[0].Result);
    }

    void TaskCoreAgentMutationTests::FailedResultEventRollsBackMutationVersionEventAndReceipt()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        const auto version = _startRunning(store, run);

        auto database = OpenRawDatabase(path.Path());
        constexpr auto createTrigger = R"SQL(
CREATE TRIGGER fail_task_run_result_event
BEFORE INSERT ON task_event
WHEN NEW.type = 'TaskRunResultUpdated'
BEGIN
    SELECT RAISE(ABORT, 'injected TaskRunResultUpdated failure');
END;
)SQL";
        VERIFY_ARE_EQUAL(
            SQLITE_OK,
            sqlite3_exec(database, createTrigger, nullptr, nullptr, nullptr));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));

        const auto command = MakeAgentMutationCommand(
            run,
            30,
            version,
            1500,
            TaskAgentMutationKind::Result,
            "must roll back");
        const auto failed = store.ApplyTaskAgentMutation(command);
        VERIFY_IS_TRUE(failed.Status == TaskStoreCommandStatus::StorageError);

        auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        if (!snapshot)
        {
            return;
        }
        VERIFY_ARE_EQUAL(version, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, snapshot->LastEventSequence);
        VERIFY_IS_TRUE(snapshot->Runs[0].Result.empty());
        VERIFY_ARE_EQUAL(size_t{ 5 }, store.ListEvents(task.Task.Id, 0, 16).size());

        database = OpenRawDatabase(path.Path());
        VERIFY_ARE_EQUAL(int64_t{ 0 }, _receiptCount(database, command.Metadata.Command));
        VERIFY_ARE_EQUAL(
            SQLITE_OK,
            sqlite3_exec(
                database,
                "DROP TRIGGER fail_task_run_result_event;",
                nullptr,
                nullptr,
                nullptr));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));

        const auto retried = store.ApplyTaskAgentMutation(command);
        VERIFY_IS_TRUE(retried.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(version + 1, retried.CommittedTaskVersion);
        VERIFY_ARE_EQUAL(uint64_t{ 6 }, retried.LastEventSequence);
        snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(std::string{ "must roll back" }, snapshot->Runs[0].Result);
    }

    void TaskCoreAgentMutationTests::ValidationRejectsTextShapeAndBounds()
    {
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        std::string error;
        VERIFY_IS_FALSE(details::ValidateTaskAgentMutationCommand(
            MakeAgentMutationCommand(
                run,
                30,
                2,
                1200,
                TaskAgentMutationKind::StatusInProgress,
                "unexpected"),
            error));
        VERIFY_IS_FALSE(details::ValidateTaskAgentMutationCommand(
            MakeAgentMutationCommand(
                run,
                31,
                2,
                1200,
                TaskAgentMutationKind::StatusBlocked),
            error));
        VERIFY_IS_FALSE(details::ValidateTaskAgentMutationCommand(
            MakeAgentMutationCommand(
                run,
                32,
                2,
                1200,
                TaskAgentMutationKind::StatusBlocked,
                std::string(MaximumTaskBlockedReasonBytes + 1, 'x')),
            error));
        VERIFY_IS_FALSE(details::ValidateTaskAgentMutationCommand(
            MakeAgentMutationCommand(
                run,
                33,
                2,
                1200,
                TaskAgentMutationKind::Result,
                std::string(MaximumTaskRunResultBytes + 1, 'x')),
            error));
    }
}
