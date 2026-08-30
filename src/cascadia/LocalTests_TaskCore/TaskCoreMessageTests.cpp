// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskCoreStoreTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    class TaskCoreMessageTests
    {
        TEST_CLASS(TaskCoreMessageTests);

        TEST_METHOD(PendingMessagesReviseAndClaimIntoOneFollowUpRun);
        TEST_METHOD(MessagesFoldBeforeStartAndDeferAfterStarting);
        TEST_METHOD(ManualHoldDefersMessagesUntilIntentCompletes);
        TEST_METHOD(UnclaimedMessageCanBeRemovedButClaimedOutcomeIsImmutable);
        TEST_METHOD(ClaimFailureRollsBackRunMessagesAndTaskVersion);
        TEST_METHOD(EnforcesUnclaimedAndPerRunMessageLimits);
    };

    void TaskCoreMessageTests::PendingMessagesReviseAndClaimIntoOneFollowUpRun()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto firstMessage = MakeEnqueueMessageCommand(task, 1, 2, 1, 1100, "first draft");
        const auto secondMessage = MakeEnqueueMessageCommand(task, 2, 4, 3, 1300, "second message");
        const auto claim = MakeClaimMessagesCommand(task, 1, 5, 4, 1400);
        {
            SqliteTaskStore store{ path.Path() };
            VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
            VERIFY_IS_TRUE(store.EnqueueTaskMessage(firstMessage).Succeeded());
            VERIFY_IS_TRUE(store.ReviseTaskMessage(
                                    MakeReviseMessageCommand(
                                        task,
                                        firstMessage.Message.Id,
                                        3,
                                        2,
                                        1,
                                        1200,
                                        std::optional<std::string>{ "revised first" }))
                               .Succeeded());
            VERIFY_IS_TRUE(store.EnqueueTaskMessage(secondMessage).Succeeded());

            const auto claimed = store.ClaimTaskMessagesAndCreateRun(claim);
            VERIFY_IS_TRUE(claimed.Status == TaskStoreCommandStatus::Succeeded);
            VERIFY_ARE_EQUAL(5ull, claimed.CommittedTaskVersion);
            VERIFY_ARE_EQUAL(5ull, claimed.LastEventSequence);
            VERIFY_IS_TRUE(store.ClaimTaskMessagesAndCreateRun(claim).Status == TaskStoreCommandStatus::Replayed);

            const auto snapshot = store.LoadTask(task.Task.Id);
            VERIFY_IS_TRUE(snapshot.has_value());
            VERIFY_ARE_EQUAL(size_t{ 2 }, snapshot->Messages.size());
            VERIFY_IS_TRUE(snapshot->Messages[0].State == TaskMessageState::Claimed);
            VERIFY_ARE_EQUAL(2ull, snapshot->Messages[0].Revision);
            VERIFY_ARE_EQUAL(std::string{ "revised first" }, snapshot->Messages[0].Content);
            VERIFY_IS_TRUE(snapshot->Messages[0].ClaimedByRunId == std::optional<TaskRunId>{ claim.Run.Id });
            VERIFY_IS_TRUE(snapshot->Messages[1].State == TaskMessageState::Claimed);
            VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->Runs.size());
            VERIFY_IS_TRUE(snapshot->Runs[0].Trigger == TaskRunTrigger::FollowUp);
            VERIFY_ARE_EQUAL(size_t{ 2 }, snapshot->RunMessages.size());
            VERIFY_ARE_EQUAL(0u, snapshot->RunMessages[0].Position);
            VERIFY_ARE_EQUAL(2ull, snapshot->RunMessages[0].MessageRevision);
            VERIFY_ARE_EQUAL(std::string{ "revised first" }, snapshot->RunMessages[0].CanonicalContent);
            VERIFY_ARE_EQUAL(1u, snapshot->RunMessages[1].Position);
            VERIFY_ARE_EQUAL(std::string{ "second message" }, snapshot->RunMessages[1].CanonicalContent);

            const auto immutable = store.ReviseTaskMessage(
                MakeReviseMessageCommand(
                    task,
                    firstMessage.Message.Id,
                    6,
                    5,
                    2,
                    1500,
                    std::optional<std::string>{ "too late" }));
            VERIFY_IS_TRUE(immutable.Status == TaskStoreCommandStatus::InvalidTransition);
        }

        SqliteTaskStore reopened{ path.Path() };
        const auto restored = reopened.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(restored.has_value());
        VERIFY_ARE_EQUAL(size_t{ 2 }, restored->RunMessages.size());
        VERIFY_ARE_EQUAL(std::string{ "revised first" }, restored->RunMessages[0].CanonicalContent);
        VERIFY_ARE_EQUAL(5ull, restored->Task.Version);
    }

    void TaskCoreMessageTests::MessagesFoldBeforeStartAndDeferAfterStarting()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto initialRun = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        const auto folded = MakeEnqueueMessageCommand(task, 1, 3, 2, 1200, "fold before provider start");
        const auto deferred = MakeEnqueueMessageCommand(task, 2, 6, 5, 1500, "run later");
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(initialRun).Succeeded());
        VERIFY_IS_TRUE(store.EnqueueTaskMessage(folded).Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                MakeRunStateCommand(initialRun, 4, 3, 1300, TaskRunState::Preparing))
                           .Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                MakeRunStateCommand(initialRun, 5, 4, 1400, TaskRunState::Starting))
                           .Succeeded());
        VERIFY_IS_TRUE(store.EnqueueTaskMessage(deferred).Succeeded());

        auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Messages[0].State == TaskMessageState::Claimed);
        VERIFY_IS_TRUE(snapshot->Messages[0].ClaimedByRunId == std::optional<TaskRunId>{ initialRun.Run.Id });
        VERIFY_IS_TRUE(snapshot->Messages[1].State == TaskMessageState::Deferred);
        VERIFY_IS_FALSE(snapshot->Messages[1].ClaimedByRunId.has_value());
        VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->RunMessages.size());

        VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                MakeRunStateCommand(initialRun, 7, 6, 1600, TaskRunState::Cancelled, "", "user-stop"))
                           .Succeeded());
        const auto followUp = MakeClaimMessagesCommand(task, 2, 8, 7, 1700);
        VERIFY_IS_TRUE(store.ClaimTaskMessagesAndCreateRun(followUp).Succeeded());

        snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(size_t{ 2 }, snapshot->Runs.size());
        VERIFY_ARE_EQUAL(2ull, snapshot->Runs[1].Sequence);
        VERIFY_IS_TRUE(snapshot->Messages[1].ClaimedByRunId == std::optional<TaskRunId>{ followUp.Run.Id });
        VERIFY_ARE_EQUAL(size_t{ 2 }, snapshot->RunMessages.size());
        VERIFY_IS_TRUE(snapshot->RunMessages[1].Run == followUp.Run.Id);
        VERIFY_ARE_EQUAL(0u, snapshot->RunMessages[1].Position);
    }

    void TaskCoreMessageTests::ManualHoldDefersMessagesUntilIntentCompletes()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto closeIntent = MakeStartIntentCommand(task, 1, 2, 1, 1100, TaskIntentKind::Close);
        const auto message = MakeEnqueueMessageCommand(task, 1, 3, 2, 1200, "wait for explicit resume");
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.StartTaskIntent(closeIntent).Succeeded());
        VERIFY_IS_TRUE(store.EnqueueTaskMessage(message).Succeeded());

        const auto blocked = store.ClaimTaskMessagesAndCreateRun(
            MakeClaimMessagesCommand(task, 1, 4, 3, 1300));
        VERIFY_IS_TRUE(blocked.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(blocked.Transition == TransitionError::ManualHoldActive);

        VERIFY_IS_TRUE(store.AdvanceTaskIntent(
                                MakeAdvanceIntentCommand(closeIntent, 5, 3, 1, 1400, TaskIntentStage::Completed))
                           .Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskManualHold(
                                MakeManualHoldCommand(task, 6, 4, 1500, false))
                           .Succeeded());
        const auto claimed = store.ClaimTaskMessagesAndCreateRun(
            MakeClaimMessagesCommand(task, 1, 7, 5, 1600));
        VERIFY_IS_TRUE(claimed.Status == TaskStoreCommandStatus::Succeeded);

        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Messages[0].State == TaskMessageState::Claimed);
        VERIFY_ARE_EQUAL(6ull, snapshot->Task.Version);
    }

    void TaskCoreMessageTests::UnclaimedMessageCanBeRemovedButClaimedOutcomeIsImmutable()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto message = MakeEnqueueMessageCommand(task, 1, 2, 1, 1100, "remove me");
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.EnqueueTaskMessage(message).Succeeded());

        const auto stale = store.ReviseTaskMessage(
            MakeReviseMessageCommand(task, message.Message.Id, 3, 2, 2, 1200, std::nullopt, true));
        VERIFY_IS_TRUE(stale.Status == TaskStoreCommandStatus::VersionConflict);
        VERIFY_IS_TRUE(stale.Transition == TransitionError::MessageRevisionMismatch);

        const auto removed = store.ReviseTaskMessage(
            MakeReviseMessageCommand(task, message.Message.Id, 4, 2, 1, 1300, std::nullopt, true));
        VERIFY_IS_TRUE(removed.Status == TaskStoreCommandStatus::Succeeded);
        const auto noMessages = store.ClaimTaskMessagesAndCreateRun(
            MakeClaimMessagesCommand(task, 1, 5, 3, 1400));
        VERIFY_IS_TRUE(noMessages.Status == TaskStoreCommandStatus::NotFound);

        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->Messages[0].State == TaskMessageState::Removed);
        VERIFY_ARE_EQUAL(2ull, snapshot->Messages[0].Revision);
        VERIFY_IS_TRUE(snapshot->Messages[0].Content.empty());
        VERIFY_ARE_EQUAL(size_t{ 0 }, snapshot->Runs.size());
        VERIFY_ARE_EQUAL(3ull, snapshot->Task.Version);
    }

    void TaskCoreMessageTests::ClaimFailureRollsBackRunMessagesAndTaskVersion()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        const auto first = MakeEnqueueMessageCommand(task, 1, 2, 1, 1100, "first");
        const auto second = MakeEnqueueMessageCommand(task, 2, 3, 2, 1200, "second");
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.EnqueueTaskMessage(first).Succeeded());
        VERIFY_IS_TRUE(store.EnqueueTaskMessage(second).Succeeded());

        auto claim = MakeClaimMessagesCommand(task, 1, 4, 3, 1300);
        const auto uniqueEvent = claim.Metadata.Event;
        claim.Metadata.Event = task.Metadata.Event;
        const auto failed = store.ClaimTaskMessagesAndCreateRun(claim);
        VERIFY_IS_TRUE(failed.Status == TaskStoreCommandStatus::StorageError);

        auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_FALSE(snapshot->ExecutionContexts.front().IdentityFrozen);
        VERIFY_ARE_EQUAL(size_t{ 0 }, snapshot->Runs.size());
        VERIFY_ARE_EQUAL(size_t{ 0 }, snapshot->RunMessages.size());
        VERIFY_IS_TRUE(snapshot->Messages[0].State == TaskMessageState::Pending);
        VERIFY_IS_TRUE(snapshot->Messages[1].State == TaskMessageState::Pending);
        VERIFY_ARE_EQUAL(3ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(3ull, snapshot->LastEventSequence);

        claim.Metadata.Event = uniqueEvent;
        VERIFY_IS_TRUE(store.ClaimTaskMessagesAndCreateRun(claim).Succeeded());
        snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->ExecutionContexts.front().IdentityFrozen);
        VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->Runs.size());
        VERIFY_ARE_EQUAL(size_t{ 2 }, snapshot->RunMessages.size());
        VERIFY_ARE_EQUAL(4ull, snapshot->Task.Version);
    }

    void TaskCoreMessageTests::EnforcesUnclaimedAndPerRunMessageLimits()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());

        auto oversized = MakeEnqueueMessageCommand(task, 999, 999, 1, 1050, "oversized");
        oversized.Message.Content.assign(MaximumTaskMessageBytes + 1, 'x');
        VERIFY_IS_TRUE(store.EnqueueTaskMessage(oversized).Status == TaskStoreCommandStatus::InvalidRecord);

        for (size_t index = 0; index < MaximumUnclaimedTaskMessages; ++index)
        {
            const auto message = MakeEnqueueMessageCommand(
                task,
                1000 + index,
                1000 + index,
                1 + index,
                1100 + static_cast<int64_t>(index),
                "bounded message");
            VERIFY_IS_TRUE(store.EnqueueTaskMessage(message).Succeeded());
        }

        const auto overflow = store.EnqueueTaskMessage(
            MakeEnqueueMessageCommand(task, 2000, 2000, 257, 1400, "one too many"));
        VERIFY_IS_TRUE(overflow.Status == TaskStoreCommandStatus::CapacityExceeded);

        const auto claim = MakeClaimMessagesCommand(task, 1, 2001, 257, 1500);
        VERIFY_IS_TRUE(store.ClaimTaskMessagesAndCreateRun(claim).Succeeded());

        const auto foldedOverflow = store.EnqueueTaskMessage(
            MakeEnqueueMessageCommand(task, 2002, 2002, 258, 1600, "cannot exceed run input bound"));
        VERIFY_IS_TRUE(foldedOverflow.Status == TaskStoreCommandStatus::CapacityExceeded);

        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(MaximumUnclaimedTaskMessages, snapshot->Messages.size());
        VERIFY_ARE_EQUAL(MaximumTaskMessagesPerRun, snapshot->RunMessages.size());
        VERIFY_ARE_EQUAL(258ull, snapshot->Task.Version);
    }
}
