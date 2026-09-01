// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskCoreStoreTestFixtures.h"
#include "TaskGitCheckpointStatePolicy.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    namespace
    {
        struct ReadyWorktreeFixture
        {
            CreateTaskCommand Task;
            CreateTaskRunCommand Run;
            uint64_t TaskVersion{};
            uint64_t WorktreeRevision{};
        };

        CreateTaskCommand _worktreeTask(const uint64_t seed)
        {
            auto task = MakeCreateTaskCommand(seed, seed, 1, 1000);
            task.ExecutionContext.Mode = ExecutionMode::Worktree;
            task.ExecutionContext.SourceRoot =
                "C:\\src\\checkpoint-" + std::to_string(seed);
            task.ExecutionContext.RepositoryCommonDirectory =
                task.ExecutionContext.SourceRoot + "\\.git";
            task.ExecutionContext.WorktreePath =
                "C:\\task-worktrees\\" + task.Task.Id.Value();
            task.ExecutionContext.RuntimeRoot = task.ExecutionContext.WorktreePath;
            task.ExecutionContext.Branch =
                "refs/heads/wt-task/" + task.Task.Id.Value();
            return task;
        }

        ReadyWorktreeFixture _ready(SqliteTaskStore& store, const uint64_t seed)
        {
            ReadyWorktreeFixture fixture;
            fixture.Task = _worktreeTask(seed);
            fixture.Run = MakeCreateRunCommand(fixture.Task, seed, 2, 1, 1100);
            VERIFY_IS_TRUE(store.CreateTask(fixture.Task).Succeeded());
            VERIFY_IS_TRUE(store.CreateTaskRun(fixture.Run).Succeeded());
            VERIFY_IS_TRUE(store.InitializeTaskGitWorktree(
                                    MakeInitializeGitWorktreeCommand(
                                        fixture.Task,
                                        3,
                                        2,
                                        1200))
                               .Succeeded());
            const std::string snapshot(64, 'c');
            uint64_t taskVersion{ 3 };
            uint64_t worktreeRevision{ 1 };
            const std::array setupStates{
                TaskGitWorktreeState::CapturingSource,
                TaskGitWorktreeState::Provisioning,
                TaskGitWorktreeState::RestoringSource,
                TaskGitWorktreeState::CreatingBaseline,
                TaskGitWorktreeState::Ready,
            };
            for (size_t index = 0; index < setupStates.size(); ++index)
            {
                const auto state = setupStates[index];
                auto advance = MakeAdvanceGitWorktreeCommand(
                    fixture.Task,
                    4 + index,
                    taskVersion,
                    worktreeRevision,
                    1300 + static_cast<int64_t>(index) * 100,
                    state,
                    state == TaskGitWorktreeState::CapturingSource ?
                        std::string{} : snapshot,
                    state == TaskGitWorktreeState::Ready ?
                        std::string(40, 'd') : std::string{},
                    state == TaskGitWorktreeState::RestoringSource ||
                        state == TaskGitWorktreeState::CreatingBaseline,
                    state == TaskGitWorktreeState::RestoringSource ||
                            state == TaskGitWorktreeState::CreatingBaseline ?
                        fixture.Task.ExecutionContext.WorktreePath :
                        std::string{});
                VERIFY_IS_TRUE(store.AdvanceTaskGitWorktree(advance).Succeeded());
                ++taskVersion;
                ++worktreeRevision;
            }
            fixture.TaskVersion = taskVersion;
            fixture.WorktreeRevision = worktreeRevision;
            return fixture;
        }

        void _terminalRun(
            SqliteTaskStore& store,
            const CreateTaskRunCommand& run,
            uint64_t& taskVersion,
            uint64_t commandSeed,
            int64_t timestamp,
            const TaskRunState outcome)
        {
            for (const auto state : {
                     TaskRunState::Preparing,
                     TaskRunState::Starting,
                     TaskRunState::Running,
                 })
            {
                VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                        MakeRunStateCommand(
                                            run,
                                            commandSeed++,
                                            taskVersion++,
                                            timestamp++,
                                            state))
                                   .Succeeded());
            }
            VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                    MakeRunStateCommand(
                                        run,
                                        commandSeed,
                                        taskVersion++,
                                        timestamp,
                                        outcome,
                                        outcome == TaskRunState::Completed ?
                                            "done" : std::string{},
                                        outcome == TaskRunState::Completed ?
                                            std::string{} : "test-outcome"))
                               .Succeeded());
        }

        void _executeSql(
            const std::filesystem::path& path,
            const std::string_view sql)
        {
            auto database = OpenRawDatabase(path);
            VERIFY_ARE_EQUAL(
                SQLITE_OK,
                sqlite3_exec(database, sql.data(), nullptr, nullptr, nullptr));
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
        }
    }

    class TaskCoreGitCheckpointTests
    {
        TEST_CLASS(TaskCoreGitCheckpointTests);

        TEST_METHOD(CheckpointPolicyAllowsDurableForwardAndRetryEdges);
        TEST_METHOD(PersistsCheckpointAndReusesReadyWorktreeAcrossRuns);
        TEST_METHOD(RecoversFailedCheckpointAndBlocksNextRunUntilResolved);
        TEST_METHOD(RollsBackCheckpointAndWorktreeEvidenceTogether);
    };

    void TaskCoreGitCheckpointTests::CheckpointPolicyAllowsDurableForwardAndRetryEdges()
    {
        VERIFY_IS_TRUE(EvaluateTaskGitCheckpointTransition(
                           TaskGitCheckpointState::Pending,
                           TaskGitCheckpointState::Inspecting)
                           .Allowed);
        VERIFY_IS_TRUE(EvaluateTaskGitCheckpointTransition(
                           TaskGitCheckpointState::Inspecting,
                           TaskGitCheckpointState::Committing)
                           .Allowed);
        VERIFY_IS_TRUE(EvaluateTaskGitCheckpointTransition(
                           TaskGitCheckpointState::Inspecting,
                           TaskGitCheckpointState::NotNeeded)
                           .Allowed);
        VERIFY_IS_TRUE(EvaluateTaskGitCheckpointTransition(
                           TaskGitCheckpointState::Committing,
                           TaskGitCheckpointState::Succeeded)
                           .Allowed);
        VERIFY_IS_TRUE(EvaluateTaskGitCheckpointTransition(
                           TaskGitCheckpointState::Failed,
                           TaskGitCheckpointState::Committing)
                           .Allowed);
        VERIFY_IS_FALSE(EvaluateTaskGitCheckpointTransition(
                            TaskGitCheckpointState::Pending,
                            TaskGitCheckpointState::Succeeded)
                            .Allowed);
        VERIFY_IS_FALSE(EvaluateTaskGitCheckpointTransition(
                            TaskGitCheckpointState::Succeeded,
                            TaskGitCheckpointState::Inspecting)
                            .Allowed);
    }

    void TaskCoreGitCheckpointTests::PersistsCheckpointAndReusesReadyWorktreeAcrossRuns()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        auto fixture = _ready(store, 201);
        auto begin = MakeBeginGitCheckpointCommand(
            fixture.Run,
            1,
            20,
            fixture.TaskVersion,
            2000);
        const auto started = store.BeginTaskRunGitCheckpoint(begin);
        VERIFY_IS_TRUE(started.Succeeded());
        ++fixture.TaskVersion;
        VERIFY_IS_TRUE(
            store.BeginTaskRunGitCheckpoint(begin).Status ==
            TaskStoreCommandStatus::Replayed);

        _terminalRun(
            store,
            fixture.Run,
            fixture.TaskVersion,
            21,
            2100,
            TaskRunState::Completed);
        auto inspecting = MakeAdvanceGitCheckpointCommand(
            begin,
            30,
            fixture.TaskVersion,
            1,
            fixture.WorktreeRevision,
            2200,
            TaskGitCheckpointState::Inspecting);
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(inspecting).Succeeded());
        ++fixture.TaskVersion;
        ++fixture.WorktreeRevision;

        auto committing = MakeAdvanceGitCheckpointCommand(
            begin,
            31,
            fixture.TaskVersion,
            2,
            fixture.WorktreeRevision,
            2300,
            TaskGitCheckpointState::Committing);
        committing.EndCommit.assign(40, 'e');
        committing.ChangedPathsJson = "[\"tracked.txt\"]";
        committing.DiffSummaryJson =
            "{\"startStateDigest\":\"" + std::string(64, 'e') +
            "\",\"files\":1}";
        committing.HasUndeliveredWork = true;
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(committing).Succeeded());
        ++fixture.TaskVersion;
        ++fixture.WorktreeRevision;

        auto succeeded = committing;
        succeeded.Metadata = MakeMetadata(32, fixture.TaskVersion, 2400);
        succeeded.ExpectedCheckpointRevision = 3;
        succeeded.ExpectedWorktreeRevision = fixture.WorktreeRevision;
        succeeded.RequestedState = TaskGitCheckpointState::Succeeded;
        succeeded.EndCommit.assign(40, 'f');
        succeeded.CheckpointCommit = succeeded.EndCommit;
        succeeded.AutoCheckpointed = true;
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(succeeded).Succeeded());
        ++fixture.TaskVersion;
        ++fixture.WorktreeRevision;

        const auto snapshot = store.LoadTask(fixture.Task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(size_t{ 1 }, snapshot->GitCheckpoints.size());
        VERIFY_IS_TRUE(
            snapshot->GitCheckpoints.front().State ==
            TaskGitCheckpointState::Succeeded);
        VERIFY_ARE_EQUAL(
            succeeded.CheckpointCommit,
            snapshot->GitCheckpoints.front().CheckpointCommit);
        VERIFY_IS_TRUE(snapshot->GitWorktree->HasUndeliveredWork);
        VERIFY_IS_TRUE(
            snapshot->GitWorktree->State == TaskGitWorktreeState::Ready);
        VERIFY_ARE_EQUAL(
            fixture.Task.ExecutionContext.WorktreePath,
            snapshot->GitWorktree->WorktreePath);

        auto nextRun = MakeCreateRunCommand(
            fixture.Task,
            202,
            33,
            fixture.TaskVersion,
            2500);
        VERIFY_IS_TRUE(store.CreateTaskRun(nextRun).Succeeded());
        const auto reused = store.LoadTask(fixture.Task.Task.Id);
        VERIFY_ARE_EQUAL(
            fixture.Task.ExecutionContext.WorktreePath,
            reused->GitWorktree->WorktreePath);
        VERIFY_ARE_EQUAL(
            fixture.Task.ExecutionContext.Branch,
            reused->GitWorktree->BranchRef);
    }

    void TaskCoreGitCheckpointTests::RecoversFailedCheckpointAndBlocksNextRunUntilResolved()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        auto fixture = _ready(store, 202);
        auto begin = MakeBeginGitCheckpointCommand(
            fixture.Run,
            2,
            40,
            fixture.TaskVersion,
            2000);
        VERIFY_IS_TRUE(store.BeginTaskRunGitCheckpoint(begin).Succeeded());
        ++fixture.TaskVersion;
        _terminalRun(
            store,
            fixture.Run,
            fixture.TaskVersion,
            41,
            2100,
            TaskRunState::Failed);

        auto inspecting = MakeAdvanceGitCheckpointCommand(
            begin,
            50,
            fixture.TaskVersion,
            1,
            fixture.WorktreeRevision,
            2200,
            TaskGitCheckpointState::Inspecting);
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(inspecting).Succeeded());
        ++fixture.TaskVersion;
        ++fixture.WorktreeRevision;
        auto failed = MakeAdvanceGitCheckpointCommand(
            begin,
            51,
            fixture.TaskVersion,
            2,
            fixture.WorktreeRevision,
            2300,
            TaskGitCheckpointState::Failed);
        failed.HasUndeliveredWork = true;
        failed.PreservedPath = fixture.Task.ExecutionContext.WorktreePath;
        failed.ReasonCode = "git-signing-failed";
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(failed).Succeeded());
        ++fixture.TaskVersion;
        ++fixture.WorktreeRevision;

        const auto recovery = store.LoadRecoverySnapshot(16);
        VERIFY_IS_TRUE(recovery.Succeeded);
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            recovery.Snapshot.RecoverableGitCheckpoints.size());
        auto nextRun = MakeCreateRunCommand(
            fixture.Task,
            203,
            52,
            fixture.TaskVersion,
            2400);
        VERIFY_IS_TRUE(
            store.CreateTaskRun(nextRun).Status ==
            TaskStoreCommandStatus::InvalidRecord);

        auto retry = MakeAdvanceGitCheckpointCommand(
            begin,
            53,
            fixture.TaskVersion,
            3,
            fixture.WorktreeRevision,
            2500,
            TaskGitCheckpointState::Inspecting);
        retry.HasUndeliveredWork = true;
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(retry).Succeeded());
        ++fixture.TaskVersion;
        ++fixture.WorktreeRevision;
        auto noCommit = retry;
        noCommit.Metadata = MakeMetadata(54, fixture.TaskVersion, 2600);
        noCommit.ExpectedCheckpointRevision = 4;
        noCommit.ExpectedWorktreeRevision = fixture.WorktreeRevision;
        noCommit.RequestedState = TaskGitCheckpointState::NotNeeded;
        noCommit.EndCommit.assign(40, 'd');
        noCommit.HasUndeliveredWork = true;
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(noCommit).Succeeded());
    }

    void TaskCoreGitCheckpointTests::RollsBackCheckpointAndWorktreeEvidenceTogether()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        auto fixture = _ready(store, 203);
        auto begin = MakeBeginGitCheckpointCommand(
            fixture.Run,
            3,
            60,
            fixture.TaskVersion,
            2000);
        VERIFY_IS_TRUE(store.BeginTaskRunGitCheckpoint(begin).Succeeded());
        ++fixture.TaskVersion;
        _terminalRun(
            store,
            fixture.Run,
            fixture.TaskVersion,
            61,
            2100,
            TaskRunState::Interrupted);
        auto inspecting = MakeAdvanceGitCheckpointCommand(
            begin,
            70,
            fixture.TaskVersion,
            1,
            fixture.WorktreeRevision,
            2200,
            TaskGitCheckpointState::Inspecting);

        _executeSql(path.Path(), R"SQL(
CREATE TRIGGER fail_checkpoint_event
BEFORE INSERT ON task_event
WHEN NEW.type='TaskRunGitCheckpointAdvanced'
BEGIN SELECT RAISE(ABORT, 'injected checkpoint event failure'); END;
)SQL");
        VERIFY_IS_TRUE(
            store.AdvanceTaskRunGitCheckpoint(inspecting).Status ==
            TaskStoreCommandStatus::StorageError);
        auto snapshot = store.LoadTask(fixture.Task.Task.Id);
        VERIFY_ARE_EQUAL(1ull, snapshot->GitCheckpoints.front().Revision);
        VERIFY_ARE_EQUAL(
            fixture.WorktreeRevision,
            snapshot->GitWorktree->Revision);
        VERIFY_ARE_EQUAL(fixture.TaskVersion, snapshot->Task.Version);

        _executeSql(path.Path(), "DROP TRIGGER fail_checkpoint_event;");
        VERIFY_IS_TRUE(store.AdvanceTaskRunGitCheckpoint(inspecting).Succeeded());
        snapshot = store.LoadTask(fixture.Task.Task.Id);
        VERIFY_ARE_EQUAL(2ull, snapshot->GitCheckpoints.front().Revision);
        VERIFY_ARE_EQUAL(
            fixture.WorktreeRevision + 1,
            snapshot->GitWorktree->Revision);
    }
}
