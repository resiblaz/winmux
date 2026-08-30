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
        TaskStoreRuntimeActivitySnapshot _activity(SqliteTaskStore& store)
        {
            const auto result = store.LoadRuntimeActivity();
            VERIFY_IS_TRUE(result.Succeeded);
            VERIFY_IS_TRUE(result.Error.empty());
            return result.Snapshot;
        }
    }

    class TaskCoreRuntimeActivityTests
    {
        TEST_CLASS(TaskCoreRuntimeActivityTests);

        TEST_METHOD(ReportsOnlyWorkThatRequiresResidentRuntime);
    };

    void TaskCoreRuntimeActivityTests::ReportsOnlyWorkThatRequiresResidentRuntime()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.LoadRuntimeActivity().Succeeded);
        VERIFY_IS_FALSE(_activity(store).Active());
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_FALSE(_activity(store).Active());

        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        VERIFY_IS_TRUE(_activity(store).HasActiveRuns);
        VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(
                                                    run,
                                                    3,
                                                    2,
                                                    1200,
                                                    TaskRunState::Cancelled))
                           .Succeeded());
        VERIFY_IS_FALSE(_activity(store).HasActiveRuns);

        VERIFY_IS_TRUE(store.ChangeTaskTerminalState(MakeTerminalStateCommand(
                                                         task,
                                                         4,
                                                         3,
                                                         1300,
                                                         TaskTerminalState::Starting,
                                                         1,
                                                         true))
                           .Succeeded());
        VERIFY_IS_TRUE(_activity(store).HasLiveTerminals);
        VERIFY_IS_TRUE(store.ChangeTaskTerminalState(MakeTerminalStateCommand(
                                                         task,
                                                         5,
                                                         4,
                                                         1400,
                                                         TaskTerminalState::Error,
                                                         1))
                           .Succeeded());
        VERIFY_IS_FALSE(_activity(store).HasLiveTerminals);

        const auto message = MakeEnqueueMessageCommand(
            task,
            1,
            6,
            5,
            1500,
            "pending work");
        VERIFY_IS_TRUE(store.EnqueueTaskMessage(message).Succeeded());
        VERIFY_IS_TRUE(_activity(store).HasPendingMessages);
        VERIFY_IS_TRUE(store.ReviseTaskMessage(MakeReviseMessageCommand(
                                                   task,
                                                   message.Message.Id,
                                                   7,
                                                   6,
                                                   1,
                                                   1600,
                                                   std::nullopt,
                                                   true))
                           .Succeeded());
        VERIFY_IS_FALSE(_activity(store).HasPendingMessages);

        const auto intent = MakeStartIntentCommand(
            task,
            1,
            8,
            7,
            1700,
            TaskIntentKind::Stop);
        VERIFY_IS_TRUE(store.StartTaskIntent(intent).Succeeded());
        VERIFY_IS_TRUE(_activity(store).HasPendingIntents);
        VERIFY_IS_TRUE(store.AdvanceTaskIntent(MakeAdvanceIntentCommand(
                                                   intent,
                                                   9,
                                                   8,
                                                   1,
                                                   1800,
                                                   TaskIntentStage::Failed,
                                                   "test failure"))
                           .Succeeded());
        VERIFY_IS_FALSE(_activity(store).HasPendingIntents);
        VERIFY_IS_FALSE(_activity(store).Active());
    }
}
