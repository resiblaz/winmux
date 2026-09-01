// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskCoreStoreTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    class TaskCoreStoreRunTests
    {
        TEST_CLASS(TaskCoreStoreRunTests);

        TEST_METHOD(CreateRunAssignsSequenceAndRejectsSecondActiveRun);
        TEST_METHOD(RunTransitionsPersistImmutableTerminalOutcome);
        TEST_METHOD(TerminalGenerationAndActiveRunGuardsAreTransactional);
        TEST_METHOD(ActiveRunBlocksConflictingTaskStatus);
        TEST_METHOD(CrossTaskRunReferencesAreRejectedWithoutReceipt);
        TEST_METHOD(ExplicitRunAtomicallyClearsManualHold);
    };

    void TaskCoreStoreRunTests::CreateRunAssignsSequenceAndRejectsSecondActiveRun()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto firstRun = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        const auto secondRun = MakeCreateRunCommand(task, 2, 3, 2, 1200);

        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        const auto created = store.CreateTaskRun(firstRun);
        VERIFY_IS_TRUE(created.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(2ull, created.CommittedTaskVersion);
        VERIFY_ARE_EQUAL(2ull, created.LastEventSequence);

        const auto rejected = store.CreateTaskRun(secondRun);
        VERIFY_IS_TRUE(rejected.Status == TaskStoreCommandStatus::ActiveRunExists);

        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(2ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->Runs.size());
        VERIFY_IS_TRUE(snapshot->Runs[0].Id == firstRun.Run.Id);
        VERIFY_ARE_EQUAL(1ull, snapshot->Runs[0].Sequence);
        VERIFY_IS_TRUE(snapshot->Runs[0].State == TaskRunState::Queued);

        const auto events = store.ListEvents(task.Task.Id, 0, 10);
        VERIFY_ARE_EQUAL(size_t{ 2 }, events.size());
        VERIFY_ARE_EQUAL(std::string{ "TaskRunCreated" }, events[1].Type);
        VERIFY_IS_TRUE(events[1].RunId == std::optional<TaskRunId>{ firstRun.Run.Id });
        VERIFY_IS_TRUE(events[1].RunSequence == std::optional<uint64_t>{ 1 });
    }

    void TaskCoreStoreRunTests::RunTransitionsPersistImmutableTerminalOutcome()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto firstRun = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        {
            SqliteTaskStore store{ path.Path() };
            VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
            VERIFY_IS_TRUE(store.CreateTaskRun(firstRun).Succeeded());
            VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                    MakeRunStateCommand(firstRun, 3, 2, 1200, TaskRunState::Preparing))
                               .Succeeded());
            VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                    MakeRunStateCommand(firstRun, 4, 3, 1300, TaskRunState::Starting))
                               .Succeeded());
            VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                    MakeRunStateCommand(firstRun, 5, 4, 1400, TaskRunState::Running))
                               .Succeeded());

            const auto completed = store.ChangeTaskRunState(
                MakeRunStateCommand(firstRun, 6, 5, 1500, TaskRunState::Completed, "result text", "completed"));
            VERIFY_IS_TRUE(completed.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(6ull, completed.CommittedTaskVersion);
            VERIFY_ARE_EQUAL(6ull, completed.LastEventSequence);

            const auto rewrite = store.ChangeTaskRunState(
                MakeRunStateCommand(firstRun, 7, 6, 1600, TaskRunState::Completed, "different result", "completed"));
            VERIFY_IS_TRUE(rewrite.Status == TaskStoreCommandStatus::InvalidTransition);

            const auto sameOutcome = store.ChangeTaskRunState(
                MakeRunStateCommand(firstRun, 8, 6, 1600, TaskRunState::Completed, "result text", "completed"));
            VERIFY_IS_TRUE(sameOutcome.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(7ull, sameOutcome.CommittedTaskVersion);

            auto secondRun = MakeCreateRunCommand(task, 2, 9, 7, 1700);
            secondRun.Run.Trigger = TaskRunTrigger::ManualRetry;
            secondRun.Run.RetryOfRunId = firstRun.Run.Id;
            const auto secondCreated = store.CreateTaskRun(secondRun);
            VERIFY_IS_TRUE(secondCreated.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(8ull, secondCreated.CommittedTaskVersion);
        }

        SqliteTaskStore reopened{ path.Path() };
        const auto snapshot = reopened.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(size_t{ 2 }, snapshot->Runs.size());
        VERIFY_ARE_EQUAL(1ull, snapshot->Runs[0].Sequence);
        VERIFY_IS_TRUE(snapshot->Runs[0].State == TaskRunState::Completed);
        VERIFY_IS_TRUE(snapshot->Runs[0].StartedAtUnixMilliseconds == std::optional<int64_t>{ 1400 });
        VERIFY_IS_TRUE(snapshot->Runs[0].FinishedAtUnixMilliseconds == std::optional<int64_t>{ 1500 });
        VERIFY_ARE_EQUAL(std::string{ "result text" }, snapshot->Runs[0].Result);
        VERIFY_ARE_EQUAL(std::string{ "completed" }, snapshot->Runs[0].ReasonCode);
        VERIFY_ARE_EQUAL(2ull, snapshot->Runs[1].Sequence);
        VERIFY_IS_TRUE(snapshot->Runs[1].State == TaskRunState::Queued);
        VERIFY_IS_TRUE(snapshot->Runs[1].RetryOfRunId == std::optional<TaskRunId>{ firstRun.Run.Id });
        VERIFY_ARE_EQUAL(8ull, snapshot->LastEventSequence);
    }

    void TaskCoreStoreRunTests::TerminalGenerationAndActiveRunGuardsAreTransactional()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 5, 3, 1400);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());

        const auto starting = store.ChangeTaskTerminalState(
            MakeTerminalStateCommand(task, 2, 1, 1100, TaskTerminalState::Starting, 1, true));
        VERIFY_IS_TRUE(starting.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(2ull, starting.CommittedTaskVersion);

        const auto stale = store.ChangeTaskTerminalState(
            MakeTerminalStateCommand(task, 3, 2, 1200, TaskTerminalState::Ready, 0));
        VERIFY_IS_TRUE(stale.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(stale.Transition == TransitionError::GenerationMismatch);

        const auto ready = store.ChangeTaskTerminalState(
            MakeTerminalStateCommand(task, 4, 2, 1300, TaskTerminalState::Ready, 1));
        VERIFY_IS_TRUE(ready.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(3ull, ready.CommittedTaskVersion);
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());

        const auto activeClose = store.ChangeTaskTerminalState(
            MakeTerminalStateCommand(task, 6, 4, 1500, TaskTerminalState::Closed, 1));
        VERIFY_IS_TRUE(activeClose.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(activeClose.Transition == TransitionError::ActiveRunConflict);

        const auto cancelled = store.ChangeTaskRunState(
            MakeRunStateCommand(run, 7, 4, 1500, TaskRunState::Cancelled, "", "user-stop"));
        VERIFY_IS_TRUE(cancelled.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(5ull, cancelled.CommittedTaskVersion);

        const auto closed = store.ChangeTaskTerminalState(
            MakeTerminalStateCommand(task, 8, 5, 1600, TaskTerminalState::Closed, 1));
        VERIFY_IS_TRUE(closed.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(6ull, closed.CommittedTaskVersion);

        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Terminal.State == TaskTerminalState::Closed);
        VERIFY_ARE_EQUAL(1ull, snapshot->Terminal.Generation);
        VERIFY_ARE_EQUAL(6ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(6ull, snapshot->LastEventSequence);
    }

    void TaskCoreStoreRunTests::ActiveRunBlocksConflictingTaskStatus()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());

        const auto cancelTask = store.ChangeTaskStatus(
            MakeStatusCommand(task, 3, 2, 1200, TaskStatus::Cancelled));
        VERIFY_IS_TRUE(cancelTask.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(cancelTask.Transition == TransitionError::ActiveRunConflict);

        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Task.Status == TaskStatus::Todo);
        VERIFY_ARE_EQUAL(2ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(2ull, snapshot->LastEventSequence);
    }

    void TaskCoreStoreRunTests::CrossTaskRunReferencesAreRejectedWithoutReceipt()
    {
        TemporaryDatabasePath path;
        const auto firstTask = MakeCreateTaskCommand(1, 1, 1, 1000);
        const auto secondTask = MakeCreateTaskCommand(2, 2, 2, 1100);
        auto run = MakeCreateRunCommand(secondTask, 1, 3, 1, 1200);
        run.Run.ExecutionContext = firstTask.ExecutionContext.Id;

        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(firstTask).Succeeded());
        VERIFY_IS_TRUE(store.CreateTask(secondTask).Succeeded());

        const auto rejected = store.CreateTaskRun(run);
        VERIFY_IS_TRUE(rejected.Status == TaskStoreCommandStatus::InvalidRecord);
        auto snapshot = store.LoadTask(secondTask.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(size_t{ 0 }, snapshot->Runs.size());
        VERIFY_ARE_EQUAL(1ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(1ull, snapshot->LastEventSequence);

        auto retryWithoutSource = MakeCreateRunCommand(secondTask, 2, 4, 1, 1250);
        retryWithoutSource.Run.Trigger = TaskRunTrigger::ManualRetry;
        VERIFY_IS_TRUE(store.CreateTaskRun(retryWithoutSource).Status == TaskStoreCommandStatus::InvalidRecord);

        run.Run.ExecutionContext = secondTask.ExecutionContext.Id;
        const auto corrected = store.CreateTaskRun(run);
        VERIFY_IS_TRUE(corrected.Status == TaskStoreCommandStatus::Succeeded);
        snapshot = store.LoadTask(secondTask.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->Runs.size());
        VERIFY_ARE_EQUAL(2ull, snapshot->Task.Version);
    }

    void TaskCoreStoreRunTests::ExplicitRunAtomicallyClearsManualHold()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        const auto task = MakeCreateTaskCommand();
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskManualHold(
                                  MakeManualHoldCommand(task, 80, 1, 1100, true))
                           .Succeeded());
        auto run = MakeCreateRunCommand(task, 81, 2, 2, 1200);
        run.ClearManualHold = true;
        const auto created = store.CreateTaskRun(run);
        VERIFY_IS_TRUE(created.Succeeded());
        const auto aggregate = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(aggregate.has_value());
        VERIFY_IS_FALSE(aggregate->Task.ManualHold);
        VERIFY_ARE_EQUAL(size_t{ 1 }, aggregate->Runs.size());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Status == TaskStoreCommandStatus::Replayed);
    }
}
