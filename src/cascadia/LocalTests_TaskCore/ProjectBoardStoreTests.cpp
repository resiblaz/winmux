// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskCoreStoreTestFixtures.h"
#include "TaskCoreSchemaV4Fixture.h"
#include "TaskStoreAgentSqlite.h"
#include "TaskStoreSchema.h"
#include <unordered_set>
#include <chrono>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    namespace
    {
        ProjectCommand projectCommand(uint64_t seed, ProjectCommandKind kind, uint64_t version = 0)
        {
            ProjectCommand result;
            result.Command = MakeId<CommandId>(70000 + seed); result.PayloadHash = std::string(64, static_cast<char>('a' + seed % 6));
            result.Id = MakeId<WorkspaceId>(101); result.Kind = kind; result.ExpectedVersion = version;
            result.Name = "Project"; result.Description = "Shared background"; result.RootPath = "C:\\src\\repository";
            result.RootIdentity = std::string(64, 'a'); result.TimestampUnixMilliseconds = 1000 + static_cast<int64_t>(seed);
            return result;
        }
        TaskBoardCommand boardCommand(const CreateTaskCommand& task, uint64_t seed, uint64_t version, TaskBoardEffect effect, TaskStatus target)
        {
            TaskBoardCommand command;
            command.Metadata = MakeMetadata(1000 + seed, version, 10000 + static_cast<int64_t>(seed));
            command.Task = task.Task.Id; command.Workspace = task.Task.Workspace; command.Effect = effect; command.Target = target;
            command.NewRun = MakeId<TaskRunId>(30000 + seed); command.NewMessage = MakeId<TaskMessageId>(40000 + seed);
            command.Intent = MakeId<TaskIntentId>(50000 + seed);
            return command;
        }
        void finishIntent(const std::filesystem::path& path)
        {
            auto db = OpenRawDatabase(path);
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(db, "UPDATE task_intent SET stage=8,revision=revision+1;", nullptr, nullptr, nullptr));
            sqlite3_close_v2(db);
        }
    }

    class TaskCoreProjectBoardTests
    {
        TEST_CLASS(TaskCoreProjectBoardTests);
        TEST_METHOD(ProjectIdentityAndConfigurationSnapshotsAreStable);
        TEST_METHOD(ProjectReceiptsAndDirectoryDeduplication);
        TEST_METHOD(ArchiveGuardsLegacyAndBoardExecution);
        TEST_METHOD(OrderingUsesColumnCasWithoutChangingTaskVersion);
        TEST_METHOD(UndoRestoresOrderAndPlanningHoldWithVersionGuards);
        TEST_METHOD(TaskSessionOwnershipIsPagedAndSurvivesDeletion);
        TEST_METHOD(ProjectFollowupRunMovesToDoingAtomically);
        TEST_METHOD(StartAndReceiptFailureAreAtomic);
        TEST_METHOD(StopThenMoveRecoversWithoutCreatingAnotherRun);
        TEST_METHOD(ManualReviewAcceptanceAndReopenRemainOneTask);
        TEST_METHOD(AcceptancePatchIsAtomicAndRequiresIdleReview);
        TEST_METHOD(SummaryPaginationIncludesFiveThousandTasks);
        TEST_METHOD(MigratesVersionFiveWithoutRewritingTaskIdentity);
    };

    void TaskCoreProjectBoardTests::ProjectIdentityAndConfigurationSnapshotsAreStable()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        auto createProject = projectCommand(1, ProjectCommandKind::Create);
        const auto saved = store.ExecuteProjectCommand(createProject); VERIFY_IS_TRUE(saved.Succeeded());
        auto task = MakeCreateTaskCommand(1, 1, 1, 2000); task.ExecutionContext.ProjectVersion = saved.Version;
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        auto update = projectCommand(2, ProjectCommandKind::UpdateMetadata, saved.Version);
        update.Description = "New background for new Tasks";
        VERIFY_IS_TRUE(store.ExecuteProjectCommand(update).Succeeded());
        const auto aggregate = store.LoadTask(task.Task.Id); VERIFY_IS_TRUE(aggregate.has_value());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, aggregate->Task.Version);
        VERIFY_ARE_EQUAL(std::string{ "Shared background" }, aggregate->ExecutionContexts.front().ProjectDescriptionSnapshot);
        auto stale = MakeCreateTaskCommand(2, 1, 2, 3000); stale.ExecutionContext.ProjectVersion = 1;
        VERIFY_IS_TRUE(store.CreateTask(stale).Status == TaskStoreCommandStatus::VersionConflict);
        stale.ExecutionContext.ProjectVersion = 2;
        VERIFY_IS_TRUE(store.CreateTask(stale).Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "New background for new Tasks" }, store.LoadTask(stale.Task.Id)->ExecutionContexts.front().ProjectDescriptionSnapshot);
    }

    void TaskCoreProjectBoardTests::ProjectReceiptsAndDirectoryDeduplication()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        auto command = projectCommand(1, ProjectCommandKind::Create);
        const auto first = store.ExecuteProjectCommand(command); VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_IS_TRUE(store.ExecuteProjectCommand(command).Status == TaskStoreCommandStatus::Replayed);
        auto duplicate = projectCommand(2, ProjectCommandKind::Create); duplicate.Id = MakeId<WorkspaceId>(102);
        const auto opened = store.ExecuteProjectCommand(duplicate);
        VERIFY_IS_TRUE(opened.Succeeded()); VERIFY_IS_TRUE(opened.Existing); VERIFY_IS_TRUE(opened.Id == first.Id);
        auto conflict = command; conflict.PayloadHash = std::string(64, 'f');
        VERIFY_IS_TRUE(store.ExecuteProjectCommand(conflict).Status == TaskStoreCommandStatus::IdempotencyConflict);
        VERIFY_ARE_EQUAL(size_t{ 1 }, store.QueryProjects({}).Projects.size());
        auto other = projectCommand(3, ProjectCommandKind::Create); other.Id = MakeId<WorkspaceId>(103); other.RootIdentity = std::string(64, 'b');
        VERIFY_IS_TRUE(store.ExecuteProjectCommand(other).Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 2 }, store.QueryProjects({}).Projects.size());
    }

    void TaskCoreProjectBoardTests::ArchiveGuardsLegacyAndBoardExecution()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto task = MakeCreateTaskCommand(); VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        auto start = boardCommand(task, 1, 1, TaskBoardEffect::Start, TaskStatus::InProgress);
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(start).Succeeded());
        auto archive = projectCommand(10, ProjectCommandKind::Archive, 1);
        VERIFY_IS_TRUE(store.ExecuteProjectCommand(archive).Status == TaskStoreCommandStatus::ActiveRunExists);
        auto run = MakeCreateRunCommand(task, 1, 30, 2, 11000); run.Run.Id = start.NewRun;
        VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(run, 31, 2, 11000, TaskRunState::Cancelled)).Succeeded());
        VERIFY_IS_TRUE(store.ExecuteProjectCommand(archive).Succeeded());
        auto legacyRun = MakeCreateRunCommand(task, 2, 32, 3, 12000); legacyRun.ClearManualHold = true;
        VERIFY_IS_FALSE(store.CreateTaskRun(legacyRun).Succeeded());
        auto restore = projectCommand(11, ProjectCommandKind::Restore, 2);
        VERIFY_IS_TRUE(store.ExecuteProjectCommand(restore).Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, store.LoadTask(task.Task.Id)->Runs.size());
        VERIFY_IS_TRUE(store.QueryProjects({}).Projects.front().Id == task.Task.Workspace);
    }

    void TaskCoreProjectBoardTests::OrderingUsesColumnCasWithoutChangingTaskVersion()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        auto first = MakeCreateTaskCommand(1, 1, 1); auto second = MakeCreateTaskCommand(2, 1, 2); auto third = MakeCreateTaskCommand(3, 1, 3);
        VERIFY_IS_TRUE(store.CreateTask(first).Succeeded()); VERIFY_IS_TRUE(store.CreateTask(second).Succeeded()); VERIFY_IS_TRUE(store.CreateTask(third).Succeeded());
        const auto revision = store.QueryProjects({}).Projects.front().ColumnRevisions[1];
        auto move = boardCommand(third, 1, 0, TaskBoardEffect::Reorder, TaskStatus::Todo);
        move.ExpectedColumnRevision = revision; move.Anchor = first.Task.Id; move.Placement = BoardPlacement::Before;
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(move).Succeeded());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, store.LoadTask(third.Task.Id)->Task.Version);
        TaskSummaryQuery query; query.Workspace = first.Task.Workspace; query.Bucket = TaskStatus::Todo; query.Sort = TaskSummarySort::BoardOrder; query.Limit = 2;
        const auto page = store.QueryTaskSummaries(query); VERIFY_IS_TRUE(page.Succeeded());
        VERIFY_IS_TRUE(page.Tasks.front().Id == third.Task.Id); VERIFY_ARE_EQUAL(uint64_t{ 3 }, page.Total); VERIFY_IS_FALSE(page.NextAfterId.empty());
        auto stale = boardCommand(second, 2, 1, TaskBoardEffect::Reorder, TaskStatus::Todo); stale.ExpectedColumnRevision = revision;
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(stale).Status == TaskStoreCommandStatus::VersionConflict);
    }

    void TaskCoreProjectBoardTests::UndoRestoresOrderAndPlanningHoldWithVersionGuards()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto first = MakeCreateTaskCommand(1, 1, 1), second = MakeCreateTaskCommand(2, 1, 2), third = MakeCreateTaskCommand(3, 1, 3);
        VERIFY_IS_TRUE(store.CreateTask(first).Succeeded()); VERIFY_IS_TRUE(store.CreateTask(second).Succeeded()); VERIFY_IS_TRUE(store.CreateTask(third).Succeeded());
        TaskSummaryQuery query; query.Workspace = first.Task.Workspace; query.Bucket = TaskStatus::Todo; query.Sort = TaskSummarySort::BoardOrder;
        auto move = boardCommand(first, 70, 1, TaskBoardEffect::Reorder, TaskStatus::Todo);
        move.ExpectedColumnRevision = store.QueryTaskSummaries(query).ColumnRevision; move.Anchor = third.Task.Id; move.Placement = BoardPlacement::After;
        const auto moved = store.ExecuteTaskBoardCommand(move); VERIFY_IS_TRUE(moved.Succeeded());
        VERIFY_IS_TRUE(store.QueryTaskSummaries(query).Tasks.back().Id == first.Task.Id);
        auto undo = boardCommand(first, 71, moved.TaskVersion, TaskBoardEffect::Undo, TaskStatus::Todo); undo.UndoOf = move.Metadata.Command;
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(undo).Succeeded());
        VERIFY_IS_TRUE(store.QueryTaskSummaries(query).Tasks.front().Id == first.Task.Id);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, store.LoadTask(first.Task.Id)->Task.Version);
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(undo).Status == TaskStoreCommandStatus::Replayed);
        auto planning = boardCommand(first, 72, 1, TaskBoardEffect::MovePlanning, TaskStatus::Backlog);
        const auto planned = store.ExecuteTaskBoardCommand(planning); VERIFY_IS_TRUE(planned.Succeeded());
        VERIFY_IS_TRUE(store.LoadTask(first.Task.Id)->Task.ManualHold);
        undo = boardCommand(first, 73, planned.TaskVersion, TaskBoardEffect::Undo, TaskStatus::Todo); undo.UndoOf = planning.Metadata.Command;
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(undo).Succeeded());
        const auto restored = store.LoadTask(first.Task.Id);
        VERIFY_IS_TRUE(restored->Task.Status == TaskStatus::Todo); VERIFY_IS_FALSE(restored->Task.ManualHold); VERIFY_IS_TRUE(restored->Runs.empty());
        VERIFY_IS_TRUE(store.QueryTaskSummaries(query).Tasks.front().Id == first.Task.Id);
        move = boardCommand(third, 74, 1, TaskBoardEffect::Reorder, TaskStatus::Todo);
        move.ExpectedColumnRevision = store.QueryTaskSummaries(query).ColumnRevision; move.Anchor = first.Task.Id; move.Placement = BoardPlacement::Before;
        const auto reordered = store.ExecuteTaskBoardCommand(move); VERIFY_IS_TRUE(reordered.Succeeded());
        auto other = boardCommand(second, 75, 1, TaskBoardEffect::Reorder, TaskStatus::Todo);
        other.ExpectedColumnRevision = store.QueryTaskSummaries(query).ColumnRevision;
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(other).Succeeded());
        undo = boardCommand(third, 76, reordered.TaskVersion, TaskBoardEffect::Undo, TaskStatus::Todo); undo.UndoOf = move.Metadata.Command;
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(undo).Status == TaskStoreCommandStatus::VersionConflict);
        VERIFY_IS_TRUE(store.QueryTaskSummaries(query).Tasks.front().Id == third.Task.Id);
    }

    void TaskCoreProjectBoardTests::TaskSessionOwnershipIsPagedAndSurvivesDeletion()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto task = MakeCreateTaskCommand(); VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        auto db = OpenRawDatabase(path.Path());
        for (uint64_t i = 0; i < 101; ++i)
        {
            const auto sql = "INSERT INTO agent_session(id,task_id,provider,provider_session_id,state,is_current,created_at,updated_at) VALUES('" +
                MakeId<AgentSessionId>(4000 + i).Value() + "','" + task.Task.Id.Value() + "','codex','task-session-" + std::to_string(i) + "',0,0,1000,1000);";
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr));
        }
        const auto first = store.QueryTaskSessionIndex(0, 100); VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 100 }, first.Sessions.size()); VERIFY_IS_TRUE(first.HasMore);
        const auto next = store.QueryTaskSessionIndex(first.AfterSequence, 100); VERIFY_IS_TRUE(next.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, next.Sessions.size()); VERIFY_IS_FALSE(next.HasMore);
        VERIFY_IS_TRUE(store.QueryTaskSessionIndex(next.AfterSequence, 100).Sessions.empty());
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(db, "PRAGMA foreign_keys=ON; DELETE FROM task;", nullptr, nullptr, nullptr));
        sqlite3_close_v2(db);
        VERIFY_IS_FALSE(store.LoadTask(task.Task.Id).has_value());
        const auto retained = store.QueryTaskSessionIndex(0, 100); VERIFY_IS_TRUE(retained.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 100 }, retained.Sessions.size()); VERIFY_IS_TRUE(retained.HasMore);
    }

    void TaskCoreProjectBoardTests::ProjectFollowupRunMovesToDoingAtomically()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.ExecuteProjectCommand(projectCommand(1, ProjectCommandKind::Create)).Succeeded());
        auto task = MakeCreateTaskCommand(1, 1, 1, 2000); task.ExecutionContext.ProjectVersion = 1;
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.EnqueueTaskMessage(MakeEnqueueMessageCommand(task, 1, 2, 1, 2100, "Follow-up")).Succeeded());
        auto run = MakeClaimMessagesCommand(task, 1, 3, 2, 2200); const auto event = run.Metadata.Event;
        run.Metadata.Event = task.Metadata.Event;
        VERIFY_IS_FALSE(store.ClaimTaskMessagesAndCreateRun(run).Succeeded());
        VERIFY_IS_TRUE(store.LoadTask(task.Task.Id)->Task.Status == TaskStatus::Todo);
        VERIFY_IS_TRUE(store.LoadTask(task.Task.Id)->Runs.empty());
        run.Metadata.Event = event; VERIFY_IS_TRUE(store.ClaimTaskMessagesAndCreateRun(run).Succeeded());
        const auto started = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(started->Task.Status == TaskStatus::InProgress); VERIFY_ARE_EQUAL(size_t{ 1 }, started->Runs.size());
    }

    void TaskCoreProjectBoardTests::StartAndReceiptFailureAreAtomic()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto task = MakeCreateTaskCommand(); VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        auto raw = OpenRawDatabase(path.Path());
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(raw, "CREATE TRIGGER reject_board_receipt BEFORE INSERT ON board_command_receipt BEGIN SELECT RAISE(ABORT,'test'); END;", nullptr, nullptr, nullptr));
        sqlite3_close_v2(raw);
        auto start = boardCommand(task, 1, 1, TaskBoardEffect::Start, TaskStatus::InProgress);
        VERIFY_IS_FALSE(store.ExecuteTaskBoardCommand(start).Succeeded());
        auto state = store.LoadTask(task.Task.Id); VERIFY_IS_TRUE(state.has_value());
        VERIFY_IS_TRUE(state->Task.Status == TaskStatus::Todo); VERIFY_IS_TRUE(state->Runs.empty()); VERIFY_IS_FALSE(state->ExecutionContexts.front().IdentityFrozen);
        raw = OpenRawDatabase(path.Path()); VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(raw, "DROP TRIGGER reject_board_receipt;", nullptr, nullptr, nullptr)); sqlite3_close_v2(raw);
        const auto created = store.ExecuteTaskBoardCommand(start); VERIFY_IS_TRUE(created.Succeeded());
        const auto replay = store.ExecuteTaskBoardCommand(start); VERIFY_IS_TRUE(replay.Status == TaskStoreCommandStatus::Replayed); VERIFY_IS_TRUE(replay.Run == created.Run);
        VERIFY_ARE_EQUAL(size_t{ 1 }, store.LoadTask(task.Task.Id)->Runs.size());
    }

    void TaskCoreProjectBoardTests::StopThenMoveRecoversWithoutCreatingAnotherRun()
    {
        TemporaryDatabasePath path;
        const auto task = MakeCreateTaskCommand();
        CommandId operation;
        {
            SqliteTaskStore store{ path.Path() }; VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
            auto start = boardCommand(task, 1, 1, TaskBoardEffect::Start, TaskStatus::InProgress);
            VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(start).Succeeded());
            auto stop = boardCommand(task, 2, 2, TaskBoardEffect::StopThenMove, TaskStatus::Todo); stop.RelatedRun = start.NewRun;
            const auto pending = store.ExecuteTaskBoardCommand(stop); VERIFY_IS_TRUE(pending.Succeeded()); VERIFY_IS_TRUE(pending.Pending);
            operation = *pending.Operation;
            VERIFY_IS_TRUE(store.LoadTask(task.Task.Id)->Task.ManualHold);
            auto run = MakeCreateRunCommand(task, 1, 30, 3, 11000); run.Run.Id = start.NewRun;
            VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(run, 31, 3, 11000, TaskRunState::Cancelled)).Succeeded());
            finishIntent(path.Path());
            auto another = MakeCreateRunCommand(task, 2, 32, 4, 12000); another.ClearManualHold = true;
            VERIFY_IS_FALSE(store.CreateTaskRun(another).Succeeded());
        }
        SqliteTaskStore restored{ path.Path() }; restored.ReconcileTaskBoardOperations();
        const auto state = restored.LoadTask(task.Task.Id); VERIFY_IS_TRUE(state.has_value());
        VERIFY_IS_TRUE(state->Task.Status == TaskStatus::Todo); VERIFY_IS_TRUE(state->Task.ManualHold); VERIFY_ARE_EQUAL(size_t{ 1 }, state->Runs.size());
        const auto result = restored.QueryTaskBoardOperation(operation); VERIFY_IS_TRUE(result.Succeeded()); VERIFY_IS_FALSE(result.Pending);
        restored.ReconcileTaskBoardOperations(); VERIFY_ARE_EQUAL(state->Task.Version, restored.LoadTask(task.Task.Id)->Task.Version);
    }

    void TaskCoreProjectBoardTests::ManualReviewAcceptanceAndReopenRemainOneTask()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto task = MakeCreateTaskCommand(); VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskStatus(MakeStatusCommand(task, 2, 1, 2000, TaskStatus::InProgress)).Succeeded());
        auto submit = boardCommand(task, 1, 2, TaskBoardEffect::SubmitForReview, TaskStatus::InReview); submit.Text = "Manual delivery";
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(submit).Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "Manual delivery" }, store.LoadTask(task.Task.Id)->BoardNote);
        auto accept = boardCommand(task, 2, 3, TaskBoardEffect::MarkDone, TaskStatus::Done);
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(accept).Succeeded());
        const auto done = store.LoadTask(task.Task.Id); VERIFY_IS_TRUE(done->Task.Status == TaskStatus::Done);
        VERIFY_IS_TRUE(done->Task.AcceptanceCriteria[0].Satisfied); VERIFY_IS_TRUE(done->Intent.has_value());
        finishIntent(path.Path());
        auto reopen = boardCommand(task, 3, 4, TaskBoardEffect::ReopenOnly, TaskStatus::Todo);
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(reopen).Succeeded());
        const auto again = store.LoadTask(task.Task.Id); VERIFY_IS_TRUE(again->Task.Status == TaskStatus::Todo); VERIFY_IS_TRUE(again->Task.ManualHold); VERIFY_IS_TRUE(again->Runs.empty());
    }

    void TaskCoreProjectBoardTests::AcceptancePatchIsAtomicAndRequiresIdleReview()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto task = MakeCreateTaskCommand(); VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        auto patch = boardCommand(task, 40, 1, TaskBoardEffect::UpdateAcceptance, TaskStatus::InReview); patch.Acceptance = { { 0, true } };
        VERIFY_IS_FALSE(store.ExecuteTaskBoardCommand(patch).Succeeded());
        VERIFY_IS_TRUE(store.ChangeTaskStatus(MakeStatusCommand(task, 2, 1, 2000, TaskStatus::InProgress)).Succeeded());
        auto review = boardCommand(task, 41, 2, TaskBoardEffect::SubmitForReview, TaskStatus::InReview); review.Text = "Ready for acceptance";
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(review).Succeeded());
        patch = boardCommand(task, 42, 3, TaskBoardEffect::UpdateAcceptance, TaskStatus::InReview); patch.Acceptance = { { 0, true }, { 255, false } };
        VERIFY_IS_FALSE(store.ExecuteTaskBoardCommand(patch).Succeeded());
        VERIFY_IS_FALSE(store.LoadTask(task.Task.Id)->Task.AcceptanceCriteria[0].Satisfied);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, store.LoadTask(task.Task.Id)->Task.Version);
        patch.Acceptance = { { 0, true }, { 1, false } }; VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(patch).Succeeded());
        auto updated = store.LoadTask(task.Task.Id); VERIFY_IS_TRUE(updated->Task.AcceptanceCriteria[0].Satisfied); VERIFY_IS_FALSE(updated->Task.AcceptanceCriteria[1].Satisfied);
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(patch).Status == TaskStoreCommandStatus::Replayed);
        auto start = boardCommand(task, 43, 4, TaskBoardEffect::Continue, TaskStatus::InProgress); start.Text = "Address the remaining criterion";
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(start).Succeeded());
        patch = boardCommand(task, 44, 5, TaskBoardEffect::UpdateAcceptance, TaskStatus::InReview); patch.Acceptance = { { 1, true } };
        VERIFY_IS_TRUE(store.ExecuteTaskBoardCommand(patch).Status == TaskStoreCommandStatus::ActiveRunExists);
        VERIFY_IS_FALSE(store.LoadTask(task.Task.Id)->Task.AcceptanceCriteria[1].Satisfied);
    }

    void TaskCoreProjectBoardTests::SummaryPaginationIncludesFiveThousandTasks()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        bool created = true;
        for (uint64_t i = 0; i < 5000; ++i)
            if (!store.CreateTask(MakeCreateTaskCommand(1000 + i, 1, 1000 + i, 1000 + static_cast<int64_t>(i))).Succeeded()) { created = false; break; }
        VERIFY_IS_TRUE(created);
        TaskSummaryQuery query; query.Limit = 100;
        std::unordered_set<std::string> seen;
        for (;;)
        {
            const auto page = store.QueryTaskSummaries(query); VERIFY_IS_TRUE(page.Succeeded()); VERIFY_ARE_EQUAL(uint64_t{ 5000 }, page.Total);
            for (const auto& item : page.Tasks) seen.insert(item.Id.Value());
            if (page.NextAfterId.empty()) break;
            query.AfterId = page.NextAfterId; query.AfterValue = page.NextAfterValue;
        }
        VERIFY_ARE_EQUAL(size_t{ 5000 }, seen.size());
        query.AfterId.clear(); query.AfterValue = 0; query.Limit = 50;
        std::vector<double> timings;
        for (size_t i = 0; i < 40; ++i)
        {
            const auto started = std::chrono::steady_clock::now();
            const auto measured = store.QueryTaskSummaries(query);
            timings.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            VERIFY_IS_TRUE(measured.Succeeded()); VERIFY_ARE_EQUAL(size_t{ 50 }, measured.Tasks.size());
        }
        std::ranges::sort(timings);
        const auto report = L"5000-Task summary, first 50 rows: p95=" + std::to_wstring(timings[37]) + L" ms; max=" + std::to_wstring(timings.back()) + L" ms";
        WEX::Logging::Log::Comment(report.c_str());
    }

    void TaskCoreProjectBoardTests::MigratesVersionFiveWithoutRewritingTaskIdentity()
    {
        TemporaryDatabasePath path;
        const auto utf8 = path.Path().u8string(); sqlite3* raw{};
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_open(reinterpret_cast<const char*>(utf8.c_str()), &raw));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(raw, FrozenTaskSchemaV4, nullptr, nullptr, nullptr));
        std::string error; VERIFY_IS_TRUE(details::MigrateAgentProfileSchema(raw, error));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(raw, R"SQL(
INSERT INTO task VALUES('00000000-0000-4000-8000-000000000001','00000000-0000-4000-8000-000000000065',NULL,'legacy','keep',1,1,7,0,1000,2000);
INSERT INTO execution_context VALUES('00000000-0000-4000-8000-0000000003e9','00000000-0000-4000-8000-000000000001',1,0,'C:/legacy','C:/legacy','','','','','codex','','old instructions',0,1000,NULL,0,'');
INSERT INTO task_terminal VALUES('00000000-0000-4000-8000-0000000007d1','00000000-0000-4000-8000-000000000001',0,0,NULL,2000);
INSERT INTO workspace_agent_default VALUES('00000000-0000-4000-8000-000000000066',NULL,1);
)SQL", nullptr, nullptr, nullptr));
        sqlite3_close_v2(raw);
        SqliteTaskStore store{ path.Path() }; VERIFY_IS_TRUE(store.IsReady()); VERIFY_ARE_EQUAL(6, store.SchemaVersion());
        const auto tasks = store.LoadTask(MakeId<TaskId>(1)); VERIFY_IS_TRUE(tasks.has_value()); VERIFY_ARE_EQUAL(uint64_t{ 7 }, tasks->Task.Version);
        VERIFY_ARE_EQUAL(std::string{ "old instructions" }, tasks->ExecutionContexts.front().Instructions);
        VERIFY_ARE_EQUAL(uint64_t{}, tasks->ExecutionContexts.front().ProjectVersion);
        const auto projects = store.QueryProjects({}); VERIFY_ARE_EQUAL(size_t{ 2 }, projects.Projects.size());
        VERIFY_IS_TRUE(projects.Projects.front().Id == MakeId<WorkspaceId>(101));
    }
}
