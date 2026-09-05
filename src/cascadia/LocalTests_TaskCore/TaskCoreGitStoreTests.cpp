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
        CreateTaskCommand _makeWorktreeTask(
            const uint64_t taskSeed = 1,
            const uint64_t workspaceSeed = 1,
            const uint64_t commandSeed = 1,
            const int64_t timestamp = 1000)
        {
            auto task = MakeCreateTaskCommand(
                taskSeed,
                workspaceSeed,
                commandSeed,
                timestamp);
            task.ExecutionContext.Mode = ExecutionMode::Worktree;
            task.ExecutionContext.SourceRoot =
                "C:\\src\\repository-" + std::to_string(taskSeed);
            task.ExecutionContext.RepositoryCommonDirectory =
                task.ExecutionContext.SourceRoot + "\\.git";
            task.ExecutionContext.WorktreePath =
                "C:\\task-worktrees\\" + task.Task.Id.Value();
            task.ExecutionContext.RuntimeRoot = task.ExecutionContext.WorktreePath;
            task.ExecutionContext.Branch =
                "refs/heads/wt-task/" + task.Task.Id.Value();
            return task;
        }

        void _executeSql(
            const std::filesystem::path& path,
            const std::string& sql)
        {
            auto database = OpenRawDatabase(path);
            VERIFY_ARE_EQUAL(
                SQLITE_OK,
                sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr));
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
        }

        StartTaskIntentCommand _advanceDeleteToAggregate(
            SqliteTaskStore& store,
            const CreateTaskCommand& task,
            const uint64_t expectedTaskVersion,
            const int64_t timestamp)
        {
            const auto intent = MakeStartIntentCommand(
                task,
                1,
                50,
                expectedTaskVersion,
                timestamp,
                TaskIntentKind::Delete);
            VERIFY_IS_TRUE(store.StartTaskIntent(intent).Succeeded());
            VERIFY_IS_TRUE(store.AdvanceTaskIntent(
                                    MakeAdvanceIntentCommand(
                                        intent,
                                        51,
                                        expectedTaskVersion + 1,
                                        1,
                                        timestamp + 100,
                                        TaskIntentStage::DeletingAggregate))
                               .Succeeded());
            return intent;
        }
    }

    class TaskCoreGitStoreTests
    {
        TEST_CLASS(TaskCoreGitStoreTests);

        TEST_METHOD(FirstRunFreezesExecutionIdentityAndFailureRollsItBack);
        TEST_METHOD(InitializeIsAtomicReplayableAndLoadsTypedGitState);
        TEST_METHOD(InitializeRejectsUnfrozenDirectAndCrossTaskContexts);
        TEST_METHOD(ExistingFrozenContextsAcceptCanonicalWindowsPathSpelling);
        TEST_METHOD(DeleteRequiresFinalizedDeliveredCleanupEvidence);
    };

    void TaskCoreGitStoreTests::ExistingFrozenContextsAcceptCanonicalWindowsPathSpelling()
    {
        TemporaryDatabasePath path;
        auto task = _makeWorktreeTask();
        const auto canonical = task;
        task.ExecutionContext.RepositoryCommonDirectory = "c:/src/repository-1\\.git";
        task.ExecutionContext.WorktreePath = "c:/task-worktrees/" + task.Task.Id.Value();
        task.ExecutionContext.RuntimeRoot = task.ExecutionContext.WorktreePath;
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(MakeCreateRunCommand(task, 1, 2, 1, 1100)).Succeeded());
        auto initialize = MakeInitializeGitWorktreeCommand(canonical, 3, 2, 1200);
        initialize.Worktree.WorktreePath += "-different";
        VERIFY_IS_TRUE(store.InitializeTaskGitWorktree(initialize).Status == TaskStoreCommandStatus::InvalidRecord);
        initialize = MakeInitializeGitWorktreeCommand(canonical, 3, 2, 1200);
        VERIFY_IS_TRUE(store.InitializeTaskGitWorktree(initialize).Succeeded());
        const auto aggregate = store.LoadTask(task.Task.Id);
        VERIFY_ARE_EQUAL(task.ExecutionContext.WorktreePath, aggregate->ExecutionContexts.front().WorktreePath);
        VERIFY_ARE_EQUAL(canonical.ExecutionContext.WorktreePath, aggregate->GitWorktree->WorktreePath);
    }

    void TaskCoreGitStoreTests::FirstRunFreezesExecutionIdentityAndFailureRollsItBack()
    {
        TemporaryDatabasePath path;
        const auto task = _makeWorktreeTask();
        auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };

        auto prematurelyFrozen = task;
        prematurelyFrozen.ExecutionContext.IdentityFrozen = true;
        VERIFY_IS_TRUE(
            store.CreateTask(prematurelyFrozen).Status ==
            TaskStoreCommandStatus::InvalidRecord);
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());

        auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->ExecutionContexts.size());
        VERIFY_IS_FALSE(snapshot->ExecutionContexts.front().IdentityFrozen);

        run.Metadata.Event = task.Metadata.Event;
        const auto failed = store.CreateTaskRun(run);
        VERIFY_IS_TRUE(failed.Status == TaskStoreCommandStatus::StorageError);

        snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_FALSE(snapshot->ExecutionContexts.front().IdentityFrozen);
        VERIFY_ARE_EQUAL(size_t{ 0 }, snapshot->Runs.size());
        VERIFY_ARE_EQUAL(1ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(1ull, snapshot->LastEventSequence);

        run.Metadata.Event = MakeId<TaskEventId>(29002);
        const auto created = store.CreateTaskRun(run);
        VERIFY_IS_TRUE(created.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(2ull, created.CommittedTaskVersion);

        snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->ExecutionContexts.front().IdentityFrozen);
        VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->Runs.size());
        VERIFY_ARE_EQUAL(2ull, snapshot->LastEventSequence);
    }

    void TaskCoreGitStoreTests::InitializeIsAtomicReplayableAndLoadsTypedGitState()
    {
        TemporaryDatabasePath path;
        const auto task = _makeWorktreeTask();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());

        auto initialize = MakeInitializeGitWorktreeCommand(task, 3, 2, 1200);
        initialize.Metadata.Event = run.Metadata.Event;
        const auto failed = store.InitializeTaskGitWorktree(initialize);
        VERIFY_IS_TRUE(failed.Status == TaskStoreCommandStatus::StorageError);

        auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_FALSE(snapshot->GitWorktree.has_value());
        VERIFY_ARE_EQUAL(2ull, snapshot->Task.Version);
        VERIFY_ARE_EQUAL(2ull, snapshot->LastEventSequence);

        initialize.Metadata.Event = MakeId<TaskEventId>(29003);
        const auto initialized = store.InitializeTaskGitWorktree(initialize);
        VERIFY_IS_TRUE(initialized.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(3ull, initialized.CommittedTaskVersion);
        VERIFY_ARE_EQUAL(3ull, initialized.LastEventSequence);

        auto replay = initialize;
        const auto replayed = store.InitializeTaskGitWorktree(replay);
        VERIFY_IS_TRUE(replayed.Status == TaskStoreCommandStatus::Replayed);
        VERIFY_ARE_EQUAL(3ull, replayed.CommittedTaskVersion);

        auto conflict = replay;
        conflict.Metadata.RequestDigest.assign(64, 'f');
        VERIFY_IS_TRUE(
            store.InitializeTaskGitWorktree(conflict).Status ==
            TaskStoreCommandStatus::IdempotencyConflict);

        const auto stale = store.InitializeTaskGitWorktree(
            MakeInitializeGitWorktreeCommand(task, 4, 2, 1300));
        VERIFY_IS_TRUE(stale.Status == TaskStoreCommandStatus::VersionConflict);

        const auto duplicate = store.InitializeTaskGitWorktree(
            MakeInitializeGitWorktreeCommand(task, 5, 3, 1300));
        VERIFY_IS_TRUE(duplicate.Status == TaskStoreCommandStatus::InvalidTransition);

        const auto checkpoint = MakeId<GitCheckpointId>(4001);
        const std::string startCommit(40, 'b');
        const std::string endCommit(40, 'c');
        const auto checkpointSql =
            "INSERT INTO git_checkpoint(id,task_id,run_id,revision,state,start_commit,end_commit,checkpoint_commit,auto_checkpointed,changed_paths_json,diff_summary_json,preserved_path,reason_code,created_at,updated_at) VALUES('" +
            checkpoint.Value() + "','" + task.Task.Id.Value() + "','" + run.Run.Id.Value() +
            "',1,3,'" + startCommit + "','" + endCommit + "','" + endCommit +
            "',1,'[\"file.txt\"]','{\"files\":1}','','',1250,1250);";
        _executeSql(path.Path(), checkpointSql);

        snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(snapshot->GitWorktree.has_value());
        VERIFY_IS_TRUE(snapshot->GitWorktree->Task == task.Task.Id);
        VERIFY_IS_TRUE(
            snapshot->GitWorktree->ExecutionContext == task.ExecutionContext.Id);
        VERIFY_IS_TRUE(
            snapshot->GitWorktree->State == TaskGitWorktreeState::Planning);
        VERIFY_ARE_EQUAL(
            task.ExecutionContext.RepositoryCommonDirectory,
            snapshot->GitWorktree->RepositoryCommonDirectory);
        VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->GitCheckpoints.size());
        VERIFY_IS_TRUE(snapshot->GitCheckpoints.front().Id == checkpoint);
        VERIFY_IS_TRUE(
            snapshot->GitCheckpoints.front().State ==
            TaskGitCheckpointState::Succeeded);
        VERIFY_ARE_EQUAL(endCommit, snapshot->GitCheckpoints.front().EndCommit);
        VERIFY_ARE_EQUAL(
            std::string{ "[\"file.txt\"]" },
            snapshot->GitCheckpoints.front().ChangedPathsJson);

        const auto recovery = store.LoadRecoverySnapshot(10);
        VERIFY_IS_TRUE(recovery.Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 1 }, recovery.Snapshot.RecoverableGitWorktrees.size());
        VERIFY_IS_FALSE(recovery.Snapshot.HasMoreRecoverableGitWorktrees);
        VERIFY_IS_TRUE(
            recovery.Snapshot.RecoverableGitWorktrees.front().Task == task.Task.Id);

        const auto activity = store.LoadRuntimeActivity();
        VERIFY_IS_TRUE(activity.Succeeded);
        VERIFY_IS_TRUE(activity.Snapshot.HasGitWork);
    }

    void TaskCoreGitStoreTests::InitializeRejectsUnfrozenDirectAndCrossTaskContexts()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };

        const auto unfrozenTask = _makeWorktreeTask(1, 1, 1, 1000);
        VERIFY_IS_TRUE(store.CreateTask(unfrozenTask).Succeeded());
        const auto unfrozen = store.InitializeTaskGitWorktree(
            MakeInitializeGitWorktreeCommand(unfrozenTask, 2, 1, 1100));
        VERIFY_IS_TRUE(unfrozen.Status == TaskStoreCommandStatus::InvalidRecord);

        const auto frozenTask = _makeWorktreeTask(2, 2, 20, 2000);
        VERIFY_IS_TRUE(store.CreateTask(frozenTask).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(
                                MakeCreateRunCommand(frozenTask, 2, 21, 1, 2100))
                           .Succeeded());

        const auto otherTask = _makeWorktreeTask(3, 3, 30, 3000);
        VERIFY_IS_TRUE(store.CreateTask(otherTask).Succeeded());
        auto crossTask = MakeInitializeGitWorktreeCommand(otherTask, 31, 1, 3100);
        crossTask.Worktree.ExecutionContext = frozenTask.ExecutionContext.Id;
        crossTask.Worktree.RepositoryCommonDirectory =
            frozenTask.ExecutionContext.RepositoryCommonDirectory;
        crossTask.Worktree.WorktreePath = frozenTask.ExecutionContext.WorktreePath;
        crossTask.Worktree.BranchRef = frozenTask.ExecutionContext.Branch;
        const auto crossTaskResult = store.InitializeTaskGitWorktree(crossTask);
        VERIFY_IS_TRUE(crossTaskResult.Status == TaskStoreCommandStatus::InvalidRecord);

        const auto directTask = MakeCreateTaskCommand(4, 4, 40, 4000);
        VERIFY_IS_TRUE(store.CreateTask(directTask).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(
                                MakeCreateRunCommand(directTask, 4, 41, 1, 4100))
                           .Succeeded());
        auto direct = MakeInitializeGitWorktreeCommand(directTask, 42, 2, 4200);
        direct.Worktree.RepositoryRoot = directTask.ExecutionContext.SourceRoot;
        direct.Worktree.RepositoryCommonDirectory = "C:\\src\\repository\\.git";
        direct.Worktree.WorktreePath = "C:\\task-worktrees\\direct";
        direct.Worktree.BranchRef =
            "refs/heads/wt-task/" + directTask.Task.Id.Value();
        const auto directResult = store.InitializeTaskGitWorktree(direct);
        VERIFY_IS_TRUE(directResult.Status == TaskStoreCommandStatus::InvalidRecord);

        VERIFY_IS_FALSE(store.LoadTask(unfrozenTask.Task.Id)->GitWorktree.has_value());
        VERIFY_IS_FALSE(store.LoadTask(otherTask.Task.Id)->GitWorktree.has_value());
        VERIFY_IS_FALSE(store.LoadTask(directTask.Task.Id)->GitWorktree.has_value());
    }

    void TaskCoreGitStoreTests::DeleteRequiresFinalizedDeliveredCleanupEvidence()
    {
        TemporaryDatabasePath path;
        const auto task = _makeWorktreeTask();
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        VERIFY_IS_TRUE(store.InitializeTaskGitWorktree(
                                MakeInitializeGitWorktreeCommand(task, 3, 2, 1200))
                           .Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                MakeRunStateCommand(
                                    run,
                                    4,
                                    3,
                                    1300,
                                    TaskRunState::Cancelled,
                                    {},
                                    "user-stop"))
                           .Succeeded());

        const auto intent = _advanceDeleteToAggregate(store, task, 4, 1400);
        const auto remove = MakeDeleteTaskCommand(intent, 52, 6, 2, 1600);

        auto blocked = store.DeleteTask(remove);
        VERIFY_IS_TRUE(blocked.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(blocked.Transition == TransitionError::UndeliveredWork);

        const auto taskPredicate = " WHERE task_id='" + task.Task.Id.Value() + "';";
        _executeSql(
            path.Path(),
            "UPDATE task_git_worktree SET state=8,has_undelivered_work=1,cleanup_eligible=1,updated_at=1650" +
                taskPredicate);
        blocked = store.DeleteTask(remove);
        VERIFY_IS_TRUE(blocked.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(blocked.Transition == TransitionError::UndeliveredWork);

        _executeSql(
            path.Path(),
            "UPDATE task_git_worktree SET has_undelivered_work=0,cleanup_eligible=0,updated_at=1660" +
                taskPredicate);
        blocked = store.DeleteTask(remove);
        VERIFY_IS_TRUE(blocked.Status == TaskStoreCommandStatus::InvalidTransition);
        VERIFY_IS_TRUE(blocked.Transition == TransitionError::UndeliveredWork);

        _executeSql(
            path.Path(),
            "UPDATE task_git_worktree SET cleanup_eligible=1,updated_at=1670" +
                taskPredicate);
        const auto deleted = store.DeleteTask(remove);
        VERIFY_IS_TRUE(deleted.Status == TaskStoreCommandStatus::Succeeded);
        VERIFY_ARE_EQUAL(7ull, deleted.CommittedTaskVersion);
        VERIFY_IS_FALSE(store.LoadTask(task.Task.Id).has_value());
        VERIFY_IS_TRUE(store.LoadDeleteTombstone(task.Task.Id).has_value());

        const auto replay = store.DeleteTask(remove);
        VERIFY_IS_TRUE(replay.Status == TaskStoreCommandStatus::Replayed);
        VERIFY_ARE_EQUAL(7ull, replay.CommittedTaskVersion);
    }
}
