// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskCoreStoreTestFixtures.h"
#include "TaskCoreSchemaV4Fixture.h"
#include "TaskStoreSchema.h"
#include <unordered_set>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskCoreLocalTests
{
    namespace
    {
        AgentProfileCommand _command(uint64_t seed, AgentProfileCommandKind kind, uint64_t version, uint64_t agent = 1)
        {
            AgentProfileCommand command;
            command.Command = MakeId<CommandId>(80000 + seed); command.PayloadHash = std::string(64, static_cast<char>('a' + seed % 6));
            command.Kind = kind; command.Agent = MakeId<AgentProfileId>(90000 + agent); command.ExpectedVersion = version;
            command.TimestampUnixMilliseconds = 1000 + static_cast<int64_t>(seed * 100);
            if (kind == AgentProfileCommandKind::Create || kind == AgentProfileCommandKind::Update)
                command.Content = AgentProfileContent{ "Reviewer", "Display only", "codex", "model-a", "Review without modifying files." };
            return command;
        }
        AgentProfileRecord _get(SqliteTaskStore& store, AgentProfileId id)
        {
            AgentProfileQuery query; query.Agent = id;
            const auto result = store.QueryAgentProfiles(query);
            VERIFY_IS_TRUE(result.Succeeded()); VERIFY_ARE_EQUAL(size_t{ 1 }, result.Profiles.size()); return result.Profiles.front();
        }
        UpdateTaskExecutionAgentCommand _apply(const CreateTaskCommand& task, const AgentProfileId& agent, uint64_t revision, uint64_t taskVersion)
        {
            UpdateTaskExecutionAgentCommand command;
            command.Metadata = MakeMetadata(99, taskVersion, 10000); command.Task = task.Task.Id;
            command.Context = MakeId<ExecutionContextId>(99999); command.Agent = AgentProfileSelection{ agent, revision }; return command;
        }
        int64_t _integer(sqlite3* db, const char* sql)
        {
            sqlite3_stmt* statement{};
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &statement, nullptr));
            VERIFY_ARE_EQUAL(SQLITE_ROW, sqlite3_step(statement)); const auto value = sqlite3_column_int64(statement, 0);
            sqlite3_finalize(statement); return value;
        }
    }

    class TaskCoreAgentProfileTests
    {
        TEST_CLASS(TaskCoreAgentProfileTests);
        TEST_METHOD(VersionedCatalogReceiptsDoNotChangeTaskRevisions);
        TEST_METHOD(ArchiveClearsDefaultsAndPreservesPinnedTaskConfiguration);
        TEST_METHOD(TaskConfigurationRequiresIdleAndRetainsHistoricalContexts);
        TEST_METHOD(PendingIntentPreventsConfigurationChange);
        TEST_METHOD(UnicodeBoundsAndCatalogPaginationAreEnforced);
        TEST_METHOD(ReceiptFailureRollsBackProfileAndEventTogether);
        TEST_METHOD(MigratesV4AndRetainsAUsablePreUpgradeBackup);
    };

    void TaskCoreAgentProfileTests::VersionedCatalogReceiptsDoNotChangeTaskRevisions()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto task = MakeCreateTaskCommand(); VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        const auto create = _command(1, AgentProfileCommandKind::Create, 0);
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(create).Succeeded());
        auto update = _command(2, AgentProfileCommandKind::Update, 1); update.Content->Instructions = "New instructions";
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(update).Succeeded());
        const auto archived = store.ExecuteAgentProfileCommand(_command(3, AgentProfileCommandKind::Archive, 2));
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, archived.Version); VERIFY_ARE_EQUAL(uint64_t{ 2 }, archived.Revision);
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(_command(4, AgentProfileCommandKind::Restore, 3)).Succeeded());
        const auto replay = store.ExecuteAgentProfileCommand(create);
        VERIFY_IS_TRUE(replay.Status == TaskStoreCommandStatus::Replayed); VERIFY_ARE_EQUAL(uint64_t{ 1 }, replay.Version);
        auto collision = create; collision.PayloadHash.assign(64, 'f');
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(collision).Status == TaskStoreCommandStatus::IdempotencyConflict);
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(_command(5, AgentProfileCommandKind::Update, 1)).Status == TaskStoreCommandStatus::VersionConflict);
        VERIFY_ARE_EQUAL(std::string{ "New instructions" }, _get(store, create.Agent).Content.Instructions);
        const auto catalog = store.QueryAgentProfiles({}); VERIFY_IS_TRUE(catalog.Succeeded());
        VERIFY_ARE_EQUAL(uint64_t{ 4 }, catalog.Generation); VERIFY_IS_TRUE(catalog.Profiles.front().Content.Instructions.empty());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, store.LoadTask(task.Task.Id)->Task.Version);
        VERIFY_IS_TRUE(store.LoadTask(task.Task.Id)->Runs.empty());
    }

    void TaskCoreAgentProfileTests::ArchiveClearsDefaultsAndPreservesPinnedTaskConfiguration()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto profile = _command(1, AgentProfileCommandKind::Create, 0); VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(profile).Succeeded());
        auto task = MakeCreateTaskCommand(); task.ExecutionContext.Model.clear(); task.ExecutionContext.Instructions.clear();
        task.ExecutionContext.SourceAgentId = profile.Agent; task.ExecutionContext.SourceAgentRevision = 1;
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        auto def = _command(2, AgentProfileCommandKind::SetWorkspaceDefault, 0); def.Workspace = task.Task.Workspace;
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(def).Succeeded());
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 2000); VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        auto update = _command(3, AgentProfileCommandKind::Update, 1); update.Content->Model = "model-b"; update.Content->Instructions = "Implement changes.";
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(update).Succeeded());
        const auto closed = store.ExecuteAgentProfileCommand(_command(4, AgentProfileCommandKind::Archive, 2));
        VERIFY_IS_TRUE(closed.Succeeded()); VERIFY_ARE_EQUAL(uint32_t{ 1 }, closed.AffectedDefaults);
        AgentProfileQuery query; query.Workspace = task.Task.Workspace;
        const auto defaults = store.QueryAgentProfiles(query); VERIFY_IS_TRUE(defaults.Succeeded());
        VERIFY_IS_FALSE(defaults.Default->Agent.has_value()); VERIFY_ARE_EQUAL(uint64_t{ 2 }, defaults.Default->Version);
        const auto snapshot = store.LoadTask(task.Task.Id);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, snapshot->Task.Version);
        VERIFY_IS_TRUE(snapshot->Runs.front().State == TaskRunState::Queued);
        VERIFY_ARE_EQUAL(std::string{ "model-a" }, snapshot->ExecutionContexts.front().Model);
        VERIFY_ARE_EQUAL(profile.Content->Instructions, snapshot->ExecutionContexts.front().Instructions);
        VERIFY_ARE_EQUAL(std::string{ "Reviewer" }, snapshot->ExecutionContexts.front().AgentNameSnapshot);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, snapshot->ExecutionContexts.front().SourceAgentRevision);
        auto another = MakeCreateTaskCommand(2, 1, 3, 2000); another.ExecutionContext.Model.clear();
        another.ExecutionContext.SourceAgentId = profile.Agent; another.ExecutionContext.SourceAgentRevision = 2;
        VERIFY_IS_TRUE(store.CreateTask(another).Status == TaskStoreCommandStatus::VersionConflict);
        def.Command = MakeId<CommandId>(81234); def.ExpectedVersion = 1; def.Agent = {};
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(def).Status == TaskStoreCommandStatus::VersionConflict);
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(_command(5, AgentProfileCommandKind::Restore, 3)).Succeeded());
        VERIFY_IS_FALSE(store.QueryAgentProfiles(query).Default->Agent.has_value());
    }

    void TaskCoreAgentProfileTests::TaskConfigurationRequiresIdleAndRetainsHistoricalContexts()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto profile = _command(1, AgentProfileCommandKind::Create, 0); VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(profile).Succeeded());
        auto task = MakeCreateTaskCommand(); task.ExecutionContext.Model.clear(); task.ExecutionContext.SourceAgentId = profile.Agent; task.ExecutionContext.SourceAgentRevision = 1;
        VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        const auto run = MakeCreateRunCommand(task, 1, 2, 1, 2000); VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
        auto update = _command(2, AgentProfileCommandKind::Update, 1); update.Content->Instructions = "Implement version two";
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(update).Succeeded());
        VERIFY_IS_TRUE(store.UpdateTaskExecutionAgent(_apply(task, profile.Agent, 2, 2)).Status == TaskStoreCommandStatus::ActiveRunExists);
        VERIFY_IS_TRUE(store.ChangeTaskRunState(MakeRunStateCommand(run, 3, 2, 3000, TaskRunState::Cancelled)).Succeeded());
        const auto apply = _apply(task, profile.Agent, 2, 3); VERIFY_IS_TRUE(store.UpdateTaskExecutionAgent(apply).Succeeded());
        VERIFY_IS_TRUE(store.UpdateTaskExecutionAgent(apply).Status == TaskStoreCommandStatus::Replayed);
        auto snapshot = store.LoadTask(task.Task.Id); VERIFY_ARE_EQUAL(size_t{ 2 }, snapshot->ExecutionContexts.size());
        VERIFY_ARE_EQUAL(profile.Content->Instructions, snapshot->ExecutionContexts.front().Instructions);
        VERIFY_ARE_EQUAL(update.Content->Instructions, snapshot->ExecutionContexts.back().Instructions);
        VERIFY_ARE_EQUAL(snapshot->ExecutionContexts.front().RuntimeRoot, snapshot->ExecutionContexts.back().RuntimeRoot);
        auto retry = MakeCreateRunCommand(task, 2, 4, 4, 11000); retry.Run.RetryOfRunId = run.Run.Id; retry.Run.Trigger = TaskRunTrigger::ManualRetry;
        VERIFY_IS_TRUE(store.CreateTaskRun(retry).Succeeded());
        snapshot = store.LoadTask(task.Task.Id);
        VERIFY_IS_TRUE(snapshot->Runs.back().ExecutionContext == snapshot->ExecutionContexts.front().Id);
    }

    void TaskCoreAgentProfileTests::PendingIntentPreventsConfigurationChange()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        const auto profile = _command(1, AgentProfileCommandKind::Create, 0); VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(profile).Succeeded());
        const auto task = MakeCreateTaskCommand(); VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
        VERIFY_IS_TRUE(store.StartTaskIntent(MakeStartIntentCommand(task, 1, 2, 1, 2000, TaskIntentKind::Close)).Succeeded());
        VERIFY_IS_TRUE(store.UpdateTaskExecutionAgent(_apply(task, profile.Agent, 1, 2)).Status == TaskStoreCommandStatus::ActiveIntentExists);
        VERIFY_ARE_EQUAL(size_t{ 1 }, store.LoadTask(task.Task.Id)->ExecutionContexts.size());
    }

    void TaskCoreAgentProfileTests::UnicodeBoundsAndCatalogPaginationAreEnforced()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        for (uint64_t index = 1; index <= 3; ++index)
        {
            auto create = _command(index, AgentProfileCommandKind::Create, 0, index); create.Content->Name = "  审查员😀  ";
            VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(create).Succeeded());
            VERIFY_ARE_EQUAL(std::string{ "审查员😀" }, _get(store, create.Agent).Content.Name);
        }
        AgentProfileQuery query; query.Limit = 2;
        const auto first = store.QueryAgentProfiles(query); VERIFY_ARE_EQUAL(size_t{ 2 }, first.Profiles.size());
        VERIFY_IS_FALSE(first.NextAfterId.empty()); query.AfterId = first.NextAfterId;
        const auto next = store.QueryAgentProfiles(query); VERIFY_ARE_EQUAL(size_t{ 1 }, next.Profiles.size()); VERIFY_IS_TRUE(next.NextAfterId.empty());
        for (const auto bad : { std::string(81, 'a'), std::string{ "bad\nname" }, std::string{ "\xf0\x28\x8c\x28" } })
        {
            auto create = _command(10, AgentProfileCommandKind::Create, 0, 10); create.Content->Name = bad;
            VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(create).Status == TaskStoreCommandStatus::InvalidRecord);
        }
        auto create = _command(11, AgentProfileCommandKind::Create, 0, 11);
        create.Content->Instructions.assign(MaximumAgentProfileInstructionsBytes + 1, 'a');
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(create).Status == TaskStoreCommandStatus::InvalidRecord);
        create.Content->Instructions.clear(); create.Content->Model = std::string{ "bad\0model", 9 };
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(create).Status == TaskStoreCommandStatus::InvalidRecord);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, store.QueryAgentProfiles({}).Generation);
    }

    void TaskCoreAgentProfileTests::ReceiptFailureRollsBackProfileAndEventTogether()
    {
        TemporaryDatabasePath path; SqliteTaskStore store{ path.Path() };
        auto raw = OpenRawDatabase(path.Path());
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(raw, "CREATE TRIGGER reject_agent_receipt BEFORE INSERT ON agent_command_receipt BEGIN SELECT RAISE(ABORT,'test'); END;", nullptr, nullptr, nullptr));
        sqlite3_close_v2(raw);
        VERIFY_IS_TRUE(store.ExecuteAgentProfileCommand(_command(1, AgentProfileCommandKind::Create, 0)).Status == TaskStoreCommandStatus::StorageError);
        const auto query = store.QueryAgentProfiles({}); VERIFY_IS_TRUE(query.Succeeded()); VERIFY_IS_TRUE(query.Profiles.empty()); VERIFY_ARE_EQUAL(uint64_t{}, query.Generation);
    }

    void TaskCoreAgentProfileTests::MigratesV4AndRetainsAUsablePreUpgradeBackup()
    {
        TemporaryDatabasePath path;
        const auto prefix = path.Path().filename().wstring() + L".pre-agent-profiles-";
        std::unordered_set<std::wstring> existing;
        for (const auto& file : std::filesystem::directory_iterator(path.Path().parent_path()))
            if (file.path().filename().wstring().starts_with(prefix)) existing.insert(file.path().filename().wstring());
        const auto utf8 = path.Path().u8string(); sqlite3* raw{};
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_open(reinterpret_cast<const char*>(utf8.c_str()), &raw));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(raw, FrozenTaskSchemaV4, nullptr, nullptr, nullptr));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(raw, R"SQL(
INSERT INTO task VALUES('00000000-0000-4000-8000-000000000001','00000000-0000-4000-8000-000000000065',NULL,'legacy task','keep me',1,1,7,0,1000,2000);
INSERT INTO execution_context VALUES('00000000-0000-4000-8000-0000000003e9','00000000-0000-4000-8000-000000000001',1,0,'C:/legacy','C:/legacy','','','','','codex','legacy-model','legacy instructions',0,1000);
INSERT INTO task_terminal VALUES('00000000-0000-4000-8000-0000000007d1','00000000-0000-4000-8000-000000000001',0,0,NULL,2000);
)SQL", nullptr, nullptr, nullptr));
        sqlite3_close_v2(raw);
        SqliteTaskStore store{ path.Path() }; VERIFY_IS_TRUE(store.IsReady()); VERIFY_ARE_EQUAL(5, store.SchemaVersion());
        const auto task = store.LoadTask(MakeId<TaskId>(1)); VERIFY_IS_TRUE(task.has_value()); VERIFY_ARE_EQUAL(uint64_t{ 7 }, task->Task.Version);
        VERIFY_ARE_EQUAL(std::string{ "legacy instructions" }, task->ExecutionContexts.front().Instructions);
        VERIFY_IS_FALSE(task->ExecutionContexts.front().SourceAgentId.has_value());
        size_t backups{};
        for (const auto& file : std::filesystem::directory_iterator(path.Path().parent_path()))
        {
            if (!file.path().filename().wstring().starts_with(prefix) || existing.contains(file.path().filename().wstring()) || file.path().extension() != L".db") continue;
            raw = OpenRawDatabase(file.path()); VERIFY_ARE_EQUAL(int64_t{ 4 }, _integer(raw, "PRAGMA user_version;"));
            VERIFY_ARE_EQUAL(int64_t{ 7 }, _integer(raw, "SELECT version FROM task;")); sqlite3_close_v2(raw);
            ++backups; std::filesystem::remove(file.path());
        }
        VERIFY_ARE_EQUAL(size_t{ 1 }, backups);
    }
}
