// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "RuntimeProjectCatalogService.h"
#include "RuntimeTaskBoardService.h"
#include "RuntimeProductRouter.h"
#include "RuntimeTaskSchedulingPolicy.h"
#include "RuntimeRtaskCommandTestSupport.h"
#include "TaskCoreStoreTestFixtures.h"
#include "TaskGitTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskCoreLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        std::string pathText(const std::filesystem::path& path)
        { const auto text = path.u8string(); return { reinterpret_cast<const char*>(text.data()), text.size() }; }
    }
    class RuntimeProjectBoardServiceTests
    {
        TEST_CLASS(RuntimeProjectBoardServiceTests);
        TEST_METHOD(ProjectBindingDeduplicatesDirectoriesAndReplaysAfterDirectoryMoves);
        TEST_METHOD(ManagedTaskWorktreeCannotBecomeProjectRoot);
        TEST_METHOD(BoardStartReplaysOneRunAndSummaryPageStaysBounded);
        TEST_METHOD(ProjectBackgroundUsesTheRunConfigurationSnapshot);
        TEST_METHOD(ProjectRoutesRequireDesktopCapabilityAndRejectCancelledWork);
    };
    void RuntimeProjectBoardServiceTests::ProjectBindingDeduplicatesDirectoriesAndReplaysAfterDirectoryMoves()
    {
        TaskGitLocalTests::TestSupport::TemporaryDirectory directory;
        const auto root = directory.Path() / L"source"; std::filesystem::create_directory(root);
        auto store = std::make_shared<SqliteTaskStore>(directory.Path() / L"tasks.db");
        RuntimeProjectCatalogService projects{ store, directory.Path() };
        ProjectCatalogCommandRequest create{ MakeId<CommandId>(1).Value(), ProjectCatalogOperation::Create,
                                            MakeId<WorkspaceId>(101).Value(), 0, "Project", "Frozen background", pathText(root) };
        auto first = projects.Execute(create); VERIFY_IS_TRUE(first.Succeeded());
        auto other = create; other.CommandId = MakeId<CommandId>(2).Value(); other.ProjectId = MakeId<WorkspaceId>(102).Value();
        const auto second = projects.Execute(other); VERIFY_IS_TRUE(second.Succeeded()); VERIFY_IS_TRUE(second.Value.Existing);
        VERIFY_ARE_EQUAL(first.Value.ProjectId, second.Value.ProjectId);
        std::filesystem::rename(root, directory.Path() / L"moved");
        const auto replay = projects.Execute(create); VERIFY_IS_TRUE(replay.Succeeded()); VERIFY_IS_TRUE(replay.Value.Replayed);
        VERIFY_ARE_EQUAL(first.Value.ProjectId, replay.Value.ProjectId);
        create.Name = "different"; VERIFY_IS_TRUE(projects.Execute(create).Status == TaskStoreCommandStatus::IdempotencyConflict);
        const auto queried = projects.Query({}); VERIFY_IS_TRUE(queried.Succeeded()); VERIFY_ARE_EQUAL(size_t{ 1 }, queried.Value.Projects.size());
        VERIFY_IS_TRUE(EncodeProjectCatalogQueryResult(queried.Value).Succeeded);
    }
    void RuntimeProjectBoardServiceTests::ManagedTaskWorktreeCannotBecomeProjectRoot()
    {
        TaskGitLocalTests::TestSupport::TemporaryDirectory directory;
        const auto root = directory.Path() / L"task-worktrees" / L"v1" / L"task";
        std::filesystem::create_directories(root);
        auto store = std::make_shared<SqliteTaskStore>(directory.Path() / L"tasks.db");
        RuntimeProjectCatalogService projects{ store, directory.Path() };
        ProjectCatalogCommandRequest create{ MakeId<CommandId>(1).Value(), ProjectCatalogOperation::Create,
                                            MakeId<WorkspaceId>(101).Value(), 0, "Project", {}, pathText(root) };
        const auto result = projects.Execute(create); VERIFY_IS_FALSE(result.Succeeded());
        VERIFY_IS_TRUE(result.Error.find("managed Task Worktree") != std::string::npos);
        VERIFY_IS_TRUE(projects.Query({}).Value.Projects.empty());
    }
    void RuntimeProjectBoardServiceTests::BoardStartReplaysOneRunAndSummaryPageStaysBounded()
    {
        TemporaryDatabasePath database; auto store = std::make_shared<SqliteTaskStore>(database.Path());
        for (uint64_t i = 1; i <= 103; ++i) VERIFY_IS_TRUE(store->CreateTask(MakeCreateTaskCommand(i, 1, i)).Succeeded());
        RuntimeTaskBoardService board{ store };
        TaskBoardCommandRequest start;
        start.CommandId = MakeId<CommandId>(900).Value(); start.TaskId = MakeId<TaskId>(1).Value(); start.ProjectId = MakeId<WorkspaceId>(101).Value();
        start.ExpectedTaskVersion = 1; start.Operation = TaskBoardOperation::Start; start.Target = TaskProjectionStatus::InProgress;
        const auto first = board.Execute(start); VERIFY_IS_TRUE(first.Succeeded()); VERIFY_IS_TRUE(first.Value.RunId.has_value());
        const auto replay = board.Execute(start); VERIFY_IS_TRUE(replay.Succeeded()); VERIFY_IS_TRUE(replay.Value.Replayed);
        VERIFY_IS_TRUE(first.Value.RunId == replay.Value.RunId);
        VERIFY_ARE_EQUAL(size_t{ 1 }, store->LoadTask(TaskId{ start.TaskId })->Runs.size());
        VERIFY_IS_TRUE(EncodeTaskBoardCommandResult(first.Value).Succeeded);
        TaskSummaryQueryRequest query; query.ProjectId = start.ProjectId; query.Limit = 100;
        const auto page = board.Query(query); VERIFY_IS_TRUE(page.Succeeded()); VERIFY_ARE_EQUAL(size_t{ 100 }, page.Value.Tasks.size());
        VERIFY_ARE_EQUAL(uint64_t{ 103 }, page.Value.Total); VERIFY_IS_TRUE(EncodeTaskSummaryQueryResult(page.Value).Succeeded);
        query.AfterId = page.Value.NextAfterId; query.AfterValue = page.Value.NextAfterValue;
        const auto next = board.Query(query); VERIFY_IS_TRUE(next.Succeeded()); VERIFY_ARE_EQUAL(size_t{ 3 }, next.Value.Tasks.size());
        start.CommandId = MakeId<CommandId>(901).Value(); start.ExpectedTaskVersion = first.Value.TaskVersion;
        VERIFY_IS_FALSE(board.Execute(start).Succeeded());
    }
    void RuntimeProjectBoardServiceTests::ProjectBackgroundUsesTheRunConfigurationSnapshot()
    {
        TemporaryDatabasePath database; auto store = std::make_shared<SqliteTaskStore>(database.Path());
        auto task = MakeCreateTaskCommand(); VERIFY_IS_TRUE(store->CreateTask(task).Succeeded());
        auto aggregate = *store->LoadTask(task.Task.Id);
        auto& original = aggregate.ExecutionContexts.front(); original.ProjectVersion = 4;
        original.ProjectNameSnapshot = "Original project"; original.ProjectDescriptionSnapshot = "Original background";
        auto later = original; later.Id = MakeId<ExecutionContextId>(901); later.Revision = 2;
        later.ProjectVersion = 5; later.ProjectDescriptionSnapshot = "New project background";
        aggregate.ExecutionContexts.push_back(later);
        TaskRunRecord run; run.Id = MakeId<TaskRunId>(1); run.Task = task.Task.Id; run.ExecutionContext = task.ExecutionContext.Id;
        const auto prompt = BuildRuntimeTaskPrompt(aggregate, run); VERIFY_IS_TRUE(prompt.has_value());
        VERIFY_IS_TRUE(prompt->find("Original background") != std::string::npos);
        VERIFY_IS_TRUE(prompt->find("New project background") == std::string::npos);
    }
    void RuntimeProjectBoardServiceTests::ProjectRoutesRequireDesktopCapabilityAndRejectCancelledWork()
    {
        RuntimeRtaskCommandTestSupport::Fixture fixture;
        const auto identity = "wt-task-runtime-v1:" + std::string(64, 'c');
        const auto instance = MakeId<CommandId>(901).Value();
        auto router = RuntimeProductRouter{ std::make_shared<RuntimeFoundationRouter>(identity, instance, 42), fixture.Service, nullptr, nullptr, nullptr,
            std::make_shared<RuntimeProjectCatalogService>(fixture.Store, std::filesystem::temp_directory_path()),
            std::make_shared<RuntimeTaskBoardService>(fixture.Store) };
        RuntimeControlClientContext client{ MakeId<CommandId>(902).Value(), ProtocolClientKind::TerminalUi, AllProtocolCapabilities };
        RuntimeControlCancellationToken token{ std::make_shared<std::atomic_bool>(false) };
        ControlRequest query{ MakeId<CommandId>(903).Value(), ControlMethod::QueryProjects, EncodeProjectCatalogQueryRequest({}).Payload };
        VERIFY_IS_TRUE(router.Route(client, query, token).Succeeded);
        client.ClientKind = ProtocolClientKind::TaskCtl;
        VERIFY_IS_TRUE(router.Route(client, query, token).Error->Code == ProtocolRpcErrorCode::CapabilityRequired);
        client.ClientKind = ProtocolClientKind::TerminalUi; client.NegotiatedCapabilities = 0;
        VERIFY_IS_TRUE(router.Route(client, query, token).Error->Code == ProtocolRpcErrorCode::CapabilityRequired);
        client.NegotiatedCapabilities = AllProtocolCapabilities;
        RuntimeControlCancellationToken cancelled{ std::make_shared<std::atomic_bool>(true) };
        VERIFY_IS_TRUE(router.Route(client, query, cancelled).Error->Code == ProtocolRpcErrorCode::RuntimeUnavailable);
        query.Method = ControlMethod::TaskBoardCommand; query.PayloadJson = "{}";
        VERIFY_IS_TRUE(router.Route(client, query, token).Error->Code == ProtocolRpcErrorCode::InvalidRequest);
    }
}
