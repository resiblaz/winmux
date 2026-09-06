// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeTaskProjectionService.h"
#include "RuntimeAgentCatalogService.h"
#include "RuntimeTaskSchedulingPolicy.h"
#include "TaskCoreStoreTestFixtures.h"
#include "TaskRuntimeTestFixtures.h"
#include "TaskGitTestSupport.h"
#include "GitRepositoryInspector.h"
#include "GitWorktreeLayout.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskCoreLocalTests::TestFixtures;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        TaskUiCommandRequest _create(const uint64_t commandSeed = 1)
        {
            TaskUiCommandRequest request;
            request.CommandId = MakeId<CommandId>(commandSeed).Value();
            request.Operation = TaskUiCommandOperation::Create;
            request.TaskId = MakeId<TaskId>(100 + commandSeed).Value();
            request.Draft = TaskUiCreateDraft{};
            request.Draft->WorkspaceId = MakeId<WorkspaceId>(200).Value();
            request.Draft->Title = "Runtime Task";
            request.Draft->Description = "Persist through the Runtime";
            request.Draft->Acceptance = { "Saved", "Queryable" };
            request.Draft->SourceRoot = "C:/src/repository";
            request.Draft->ExecutionMode = TaskProjectionExecutionMode::Direct;
            request.Draft->Instructions = "Keep changes scoped.";
            return request;
        }

        TaskUiCommandRequest _action(
            const TaskUiCommandRequest& create,
            const uint64_t commandSeed,
            const TaskUiCommandOperation operation,
            const uint64_t version)
        {
            TaskUiCommandRequest request;
            request.CommandId = MakeId<CommandId>(commandSeed).Value();
            request.Operation = operation;
            request.TaskId = create.TaskId;
            request.ExpectedTaskVersion = version;
            return request;
        }
    }

    class RuntimeTaskProjectionServiceTests
    {
        TEST_CLASS(RuntimeTaskProjectionServiceTests);

        TEST_METHOD(CreateQueryStartStopAndCloseUseDurableReceipts);
        TEST_METHOD(QueryFiltersWorkspaceCompletionAndEventTail);
        TEST_METHOD(WorktreeDraftProjectsDeterministicFuturePath);
        TEST_METHOD(RejectsStaleAndInvalidCommandsWithoutMutation);
        TEST_METHOD(MessageRetryAndDerivedTaskCommandsAreDurable);
        TEST_METHOD(OpenTerminalCreatesOneGenerationAndEnforcesSurfaceOwner);
        TEST_METHOD(AgentUpdatesKeepQueuedAndRetriedRunsOnTheirOriginalConfiguration);
    };

    void RuntimeTaskProjectionServiceTests::AgentUpdatesKeepQueuedAndRetriedRunsOnTheirOriginalConfiguration()
    {
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        RuntimeAgentCatalogService agents{ store };
        RuntimeTaskProjectionService tasks{ store, nullptr, path.Path().parent_path() };
        AgentCatalogCommandRequest profile;
        profile.CommandId = MakeId<CommandId>(900).Value(); profile.AgentId = MakeId<AgentProfileId>(901).Value();
        profile.Configuration = AgentProfileConfiguration{ "Reviewer", "Not an instruction", "codex", "model-one", "Role one" };
        VERIFY_IS_TRUE(agents.Execute(profile).Succeeded());
        auto create = _create(20); create.Draft->Instructions.clear(); create.Draft->Agent = AgentProfileReference{ profile.AgentId, 1 };
        VERIFY_IS_TRUE(tasks.Execute(create, nullptr).Succeeded());
        profile.CommandId = MakeId<CommandId>(902).Value(); profile.ExpectedVersion = 1; profile.Operation = AgentCatalogOperation::Update;
        profile.Configuration->Model = "model-two"; profile.Configuration->Instructions = "Role two";
        VERIFY_IS_TRUE(agents.Execute(profile).Succeeded());
        const auto started = tasks.Execute(_action(create, 21, TaskUiCommandOperation::Start, 1), nullptr); VERIFY_IS_TRUE(started.Succeeded());
        auto aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_ARE_EQUAL(std::string{ "Role one" }, FindRuntimeTaskRunContext(*aggregate, aggregate->Runs.front())->Instructions);
        auto apply = _action(create, 22, TaskUiCommandOperation::UpdateAgent, 2);
        apply.AgentConfiguration = TaskAgentConfiguration{}; apply.AgentConfiguration->Agent = AgentProfileReference{ profile.AgentId, 2 };
        VERIFY_IS_TRUE(tasks.Execute(apply, nullptr).Status == RuntimeTaskProjectionStatus::Conflict);
        ChangeTaskRunStateCommand cancel;
        cancel.Metadata = MakeMetadata(903, 2, aggregate->Task.UpdatedAtUnixMilliseconds + 1); cancel.Task = aggregate->Task.Id;
        cancel.Run = aggregate->Runs.front().Id; cancel.RequestedState = TaskRunState::Cancelled;
        VERIFY_IS_TRUE(store->ChangeTaskRunState(cancel).Succeeded());
        apply.ExpectedTaskVersion = 3; VERIFY_IS_TRUE(tasks.Execute(apply, nullptr).Succeeded());
        auto retry = _action(create, 23, TaskUiCommandOperation::Retry, 4); retry.RelatedRunId = *started.Result.RunId;
        VERIFY_IS_TRUE(tasks.Execute(retry, nullptr).Succeeded());
        aggregate = store->LoadTask(TaskId{ create.TaskId });
        const auto& retried = aggregate->Runs.back();
        VERIFY_ARE_EQUAL(std::string{ "model-one" }, FindRuntimeTaskRunContext(*aggregate, retried)->Model);
        TaskProjectionQueryRequest query; query.TaskId = create.TaskId;
        const auto projection = tasks.Query(query); VERIFY_IS_TRUE(projection.Succeeded());
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, projection.Projection.Tasks.front().Execution.Agent->Revision);
        for (const auto& run : projection.Projection.Tasks.front().Runs) VERIFY_ARE_EQUAL(uint64_t{ 1 }, run.Agent->Revision);
        AgentSessionRecord session; session.Id = MakeId<AgentSessionId>(950);
        aggregate->Runs.front().AgentSession = session.Id; aggregate->Runs.front().StartedAtUnixMilliseconds = 1000;
        VERIFY_IS_TRUE(CanResumeRuntimeTaskAgentSession(*aggregate, retried, session));
        auto future = retried; future.Id = MakeId<TaskRunId>(951); future.ExecutionContext = aggregate->ExecutionContexts.back().Id;
        VERIFY_IS_FALSE(CanResumeRuntimeTaskAgentSession(*aggregate, future, session));
        aggregate->ExecutionContexts.back().Model = aggregate->ExecutionContexts.front().Model;
        aggregate->ExecutionContexts.back().Instructions = aggregate->ExecutionContexts.front().Instructions;
        aggregate->ExecutionContexts.back().AgentNameSnapshot = "Renamed display";
        VERIFY_IS_TRUE(CanResumeRuntimeTaskAgentSession(*aggregate, future, session));
        aggregate->ExecutionContexts.back().SourceAgentId = MakeId<AgentProfileId>(999);
        VERIFY_IS_FALSE(CanResumeRuntimeTaskAgentSession(*aggregate, future, session));
    }

    void RuntimeTaskProjectionServiceTests::CreateQueryStartStopAndCloseUseDurableReceipts()
    {
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        RuntimeTaskProjectionService service{ store, nullptr, path.Path().parent_path() };
        VERIFY_IS_TRUE(service.Valid());
        const auto create = _create();
        auto result = service.Execute(create, nullptr);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, result.Result.TaskVersion);

        const auto replay = service.Execute(create, nullptr);
        VERIFY_IS_TRUE(replay.Status == RuntimeTaskProjectionStatus::Replayed);
        VERIFY_IS_TRUE(replay.Result.Replayed);
        VERIFY_ARE_EQUAL(create.TaskId, replay.Result.TaskId);

        TaskProjectionQueryRequest query;
        query.TaskId = create.TaskId;
        query.IncludeCompleted = true;
        query.EventLimit = 32;
        auto queried = service.Query(query);
        VERIFY_IS_TRUE(queried.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, queried.Projection.Tasks.size());
        VERIFY_ARE_EQUAL(std::string{ "Runtime Task" }, queried.Projection.Tasks.front().Title);

        const auto start = _action(create, 2, TaskUiCommandOperation::Start, 1);
        result = service.Execute(start, nullptr);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(result.Result.RunId.has_value());
        VERIFY_IS_TRUE(result.Result.Pending);
        const auto originalRun = result.Result.RunId;
        const auto startReplay = service.Execute(start, nullptr);
        VERIFY_IS_TRUE(startReplay.Status == RuntimeTaskProjectionStatus::Replayed);
        VERIFY_IS_TRUE(startReplay.Result.RunId == originalRun);

        const auto stop = _action(create, 3, TaskUiCommandOperation::Stop, 2);
        result = service.Execute(stop, nullptr);
        VERIFY_IS_TRUE(result.Succeeded());
        auto aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_IS_TRUE(aggregate->Runs.front().State == TaskRunState::Queued);
        VERIFY_IS_TRUE(aggregate->Task.ManualHold);
        VERIFY_IS_TRUE(aggregate->Intent.has_value());
        VERIFY_IS_TRUE(aggregate->Intent->Kind == TaskIntentKind::Stop);

        const auto closeTask = _create(5);
        VERIFY_IS_TRUE(service.Execute(closeTask, nullptr).Succeeded());
        const auto close = _action(closeTask, 6, TaskUiCommandOperation::Close, 1);
        result = service.Execute(close, nullptr);
        VERIFY_IS_TRUE(result.Succeeded());
        aggregate = store->LoadTask(TaskId{ closeTask.TaskId });
        VERIFY_IS_TRUE(aggregate->Intent.has_value());
        VERIFY_IS_TRUE(aggregate->Intent->Kind == TaskIntentKind::Close);
        VERIFY_IS_TRUE(result.Result.Pending);
    }

    void RuntimeTaskProjectionServiceTests::QueryFiltersWorkspaceCompletionAndEventTail()
    {
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        RuntimeTaskProjectionService service{ store, nullptr, path.Path().parent_path() };
        const auto first = _create(10);
        auto second = _create(20);
        second.Draft->WorkspaceId = MakeId<WorkspaceId>(201).Value();
        VERIFY_IS_TRUE(service.Execute(first, nullptr).Succeeded());
        VERIFY_IS_TRUE(service.Execute(second, nullptr).Succeeded());

        TaskProjectionQueryRequest query;
        query.WorkspaceId = first.Draft->WorkspaceId;
        query.EventLimit = 1;
        auto result = service.Query(query);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, result.Projection.Tasks.size());
        VERIFY_ARE_EQUAL(first.TaskId, result.Projection.Tasks.front().Id);
        VERIFY_ARE_EQUAL(size_t{ 1 }, result.Projection.Tasks.front().Events.size());

        query.WorkspaceId.reset();
        result = service.Query(query);
        VERIFY_ARE_EQUAL(size_t{ 2 }, result.Projection.Tasks.size());
    }

    void RuntimeTaskProjectionServiceTests::WorktreeDraftProjectsDeterministicFuturePath()
    {
        using namespace TaskGitLocalTests::TestSupport;
        GitFixture git;
        const auto repository = git.Temporary.Path() / L"Repository";
        VERIFY_IS_TRUE(git.Initialize(repository));
        const auto dataRoot = git.Temporary.Path() / L"RuntimeData";
        std::filesystem::create_directory(dataRoot);
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        // The product identity lowercases its settings root, while Git and Win32
        // return different slash styles and the on-disk casing.
        RuntimeTaskProjectionService service{ store, nullptr, RequireIdentity(dataRoot).SettingsRoot };
        auto create = _create(30);
        create.Draft->ExecutionMode = TaskProjectionExecutionMode::Worktree;
        create.Draft->SourceRoot = Utf8Path(repository);
        VERIFY_IS_TRUE(service.Execute(create, nullptr).Succeeded());
        TaskProjectionQueryRequest query;
        query.TaskId = create.TaskId;
        query.IncludeCompleted = true;
        const auto projected = service.Query(query);
        VERIFY_IS_TRUE(projected.Succeeded());
        const auto& execution = projected.Projection.Tasks.front().Execution;
        VERIFY_IS_TRUE(execution.Mode == TaskProjectionExecutionMode::Worktree);
        VERIFY_IS_TRUE(execution.WorktreePath.find(create.TaskId) != std::string::npos);
        VERIFY_IS_TRUE(execution.Branch.find(create.TaskId) != std::string::npos);
        VERIFY_IS_TRUE(service.Execute(_action(create, 31, TaskUiCommandOperation::Start, 1), nullptr).Succeeded());
        const auto aggregate = store->LoadTask(TaskId{ create.TaskId });
        CreateTaskCommand task;
        task.Task = aggregate->Task;
        task.ExecutionContext = aggregate->ExecutionContexts.front();
        auto initialize = MakeInitializeGitWorktreeCommand(task, 32, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
        Microsoft::Terminal::TaskExecution::GitRepositoryInspector inspector{ git.GitExecutable };
        const auto inspected = inspector.Inspect(repository, git.Cancellation.Get());
        const auto layout = Microsoft::Terminal::TaskGit::PrepareGitWorktreeLayout(dataRoot, task.Task.Id);
        VERIFY_IS_TRUE(inspected.Succeeded());
        VERIFY_IS_TRUE(layout.Succeeded());
        const auto common = inspected.Inspection->CommonDirectoryIdentity.CanonicalPath().u8string();
        initialize.Worktree.RepositoryCommonDirectory.assign(reinterpret_cast<const char*>(common.data()), common.size());
        initialize.Worktree.WorktreePath = layout.Layout->WorktreePathUtf8;
        VERIFY_IS_TRUE(store->InitializeTaskGitWorktree(initialize).Succeeded());
    }

    void RuntimeTaskProjectionServiceTests::RejectsStaleAndInvalidCommandsWithoutMutation()
    {
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        RuntimeTaskProjectionService service{ store, nullptr, path.Path().parent_path() };
        const auto create = _create(40);
        VERIFY_IS_TRUE(service.Execute(create, nullptr).Succeeded());
        auto stale = _action(create, 41, TaskUiCommandOperation::Start, 99);
        VERIFY_IS_TRUE(service.Execute(stale, nullptr).Status == RuntimeTaskProjectionStatus::Conflict);
        auto aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_ARE_EQUAL(size_t{ 0 }, aggregate->Runs.size());

        stale.TaskId = "not-a-uuid";
        VERIFY_IS_TRUE(service.Execute(stale, nullptr).Status == RuntimeTaskProjectionStatus::InvalidRequest);
        TaskProjectionQueryRequest invalid;
        invalid.TaskId = create.TaskId;
        invalid.WorkspaceId = create.Draft->WorkspaceId;
        VERIFY_IS_TRUE(service.Query(invalid).Status == RuntimeTaskProjectionStatus::InvalidRequest);
    }

    void RuntimeTaskProjectionServiceTests::MessageRetryAndDerivedTaskCommandsAreDurable()
    {
        TemporaryDatabasePath path;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        RuntimeTaskProjectionService service{ store, nullptr, path.Path().parent_path() };
        const auto create = _create(60);
        VERIFY_IS_TRUE(service.Execute(create, nullptr).Succeeded());

        auto message = _action(create, 61, TaskUiCommandOperation::QueueMessage, 1);
        message.Text = "first follow-up";
        VERIFY_IS_TRUE(service.Execute(message, nullptr).Succeeded());
        auto aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_ARE_EQUAL(size_t{ 1 }, aggregate->Messages.size());
        VERIFY_IS_TRUE(aggregate->Messages.front().State == TaskMessageState::Pending);

        auto revise = _action(create, 62, TaskUiCommandOperation::ReviseMessage, aggregate->Task.Version);
        revise.MessageId = aggregate->Messages.front().Id.Value();
        revise.ExpectedMessageRevision = 1;
        revise.Text = "revised follow-up";
        VERIFY_IS_TRUE(service.Execute(revise, nullptr).Succeeded());
        aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_ARE_EQUAL(std::string{ "revised follow-up" }, aggregate->Messages.front().Content);

        auto remove = _action(create, 63, TaskUiCommandOperation::RemoveMessage, aggregate->Task.Version);
        remove.MessageId = aggregate->Messages.front().Id.Value();
        remove.ExpectedMessageRevision = 2;
        VERIFY_IS_TRUE(service.Execute(remove, nullptr).Succeeded());
        aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_IS_TRUE(aggregate->Messages.front().State == TaskMessageState::Removed);

        const auto start = _action(create, 64, TaskUiCommandOperation::Start, aggregate->Task.Version);
        const auto started = service.Execute(start, nullptr);
        VERIFY_IS_TRUE(started.Succeeded());
        aggregate = store->LoadTask(TaskId{ create.TaskId });
        ChangeTaskRunStateCommand cancel;
        cancel.Metadata = MakeMetadata(65, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds + 1);
        cancel.Task = aggregate->Task.Id;
        cancel.Run = aggregate->Runs.front().Id;
        cancel.RequestedState = TaskRunState::Cancelled;
        cancel.ReasonCode = "test";
        VERIFY_IS_TRUE(store->ChangeTaskRunState(cancel).Succeeded());
        aggregate = store->LoadTask(TaskId{ create.TaskId });
        auto retry = _action(create, 66, TaskUiCommandOperation::Retry, aggregate->Task.Version);
        retry.RelatedRunId = aggregate->Runs.front().Id.Value();
        const auto retried = service.Execute(retry, nullptr);
        VERIFY_IS_TRUE(retried.Succeeded());
        aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_ARE_EQUAL(size_t{ 2 }, aggregate->Runs.size());
        VERIFY_IS_TRUE(aggregate->Runs.back().Trigger == TaskRunTrigger::ManualRetry);
        VERIFY_IS_TRUE(aggregate->Runs.back().RetryOfRunId == std::optional<TaskRunId>{ aggregate->Runs.front().Id });

        ChangeTaskRunStateCommand cancelRetry;
        cancelRetry.Metadata = MakeMetadata(67, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds + 1);
        cancelRetry.Task = aggregate->Task.Id;
        cancelRetry.Run = aggregate->Runs.back().Id;
        cancelRetry.RequestedState = TaskRunState::Cancelled;
        cancelRetry.ReasonCode = "test";
        VERIFY_IS_TRUE(store->ChangeTaskRunState(cancelRetry).Succeeded());
        aggregate = store->LoadTask(TaskId{ create.TaskId });

        auto derived = _action(create, 68, TaskUiCommandOperation::Derive, aggregate->Task.Version);
        derived.DerivedTaskId = MakeId<TaskId>(999).Value();
        derived.Draft = *create.Draft;
        derived.Draft->Title = "Derived Task";
        const auto derivedResult = service.Execute(derived, nullptr);
        VERIFY_IS_TRUE(derivedResult.Succeeded());
        const auto derivedAggregate = store->LoadTask(TaskId{ *derived.DerivedTaskId });
        VERIFY_IS_TRUE(derivedAggregate.has_value());
        VERIFY_IS_TRUE(derivedAggregate->Task.ForkedFromTaskId == std::optional<TaskId>{ TaskId{ create.TaskId } });
    }

    void RuntimeTaskProjectionServiceTests::OpenTerminalCreatesOneGenerationAndEnforcesSurfaceOwner()
    {
        TemporaryDatabasePath path;
        UniqueRuntimeRoot runtimeRoot;
        const auto store = std::make_shared<SqliteTaskStore>(path.Path());
        const auto identity = RequireIdentity(runtimeRoot.Path());
        auto terminals = std::make_shared<RuntimeTaskTerminalService>(
            identity,
            "91000000-0000-4000-8000-000000000001",
            [](auto&&, auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; },
            [](auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; });
        RuntimeTaskProjectionService service{ store, nullptr, path.Path().parent_path(), terminals };
        const auto create = _create(70);
        VERIFY_IS_TRUE(service.Execute(create, nullptr).Succeeded());
        const auto open = _action(create, 71, TaskUiCommandOperation::OpenTerminal, 1);
        const auto opened = service.Execute(
            open, nullptr, "92000000-0000-4000-8000-000000000002");
        VERIFY_IS_TRUE(opened.Succeeded());
        VERIFY_IS_TRUE(opened.Result.Surface.has_value());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, opened.Result.Surface->Generation);
        VERIFY_IS_FALSE(opened.Result.Surface->OwnedElsewhere);
        const auto aggregate = store->LoadTask(TaskId{ create.TaskId });
        VERIFY_IS_TRUE(aggregate->Terminal.State == TaskTerminalState::Ready);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, aggregate->Terminal.Generation);

        auto competing = _action(create, 72, TaskUiCommandOperation::OpenTerminal, aggregate->Task.Version);
        const auto owned = service.Execute(
            competing, nullptr, "93000000-0000-4000-8000-000000000003");
        VERIFY_IS_TRUE(owned.Succeeded());
        VERIFY_IS_TRUE(owned.Result.Surface->OwnedElsewhere);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, owned.Result.Surface->Generation);
        terminals->Stop();
    }
}
