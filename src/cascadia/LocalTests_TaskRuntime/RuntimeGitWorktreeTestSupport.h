// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeGitWorktreeSetup.h"

#include "GitRepositoryInspector.h"
#include "GitWorktreeLayout.h"
#include "GitWorktreeTestSupport.h"
#include "TaskCoreStoreTestFixtures.h"

namespace TaskRuntimeLocalTests::GitTestSupport
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskExecution;
    using namespace Microsoft::Terminal::TaskGit;
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace TaskCoreLocalTests::TestFixtures;
    using namespace TaskGitLocalTests::TestSupport;

    struct RuntimeReadyWorktreeFixture
    {
        GitFixture Git;
        TemporaryDatabasePath Database;
        std::filesystem::path Repository;
        std::filesystem::path RuntimeRoot;
        std::filesystem::path ManagedWorktree;
        CreateTaskCommand Task;
        CreateTaskRunCommand Run;
        std::shared_ptr<SqliteTaskStore> Store;

        [[nodiscard]] TaskAggregateSnapshot Aggregate() const
        {
            const auto aggregate = Store->LoadTask(Task.Task.Id);
            VERIFY_IS_TRUE(aggregate.has_value());
            return *aggregate;
        }

        [[nodiscard]] TaskGitWorktreeRecord Worktree() const
        {
            const auto aggregate = Aggregate();
            VERIFY_IS_TRUE(aggregate.GitWorktree.has_value());
            return *aggregate.GitWorktree;
        }
    };

    inline RuntimeGitWorktreeSetupContext SetupContext(
        const uint64_t seed,
        const int64_t timestamp)
    {
        RuntimeGitWorktreeSetupContext context;
        context.Command = MakeId<CommandId>(70000 + seed);
        context.Event = MakeId<TaskEventId>(80000 + seed);
        context.UnixMilliseconds = timestamp;
        return context;
    }

    inline std::unique_ptr<RuntimeReadyWorktreeFixture> MakeReadyWorktree(
        const uint64_t seed)
    {
        auto fixture = std::make_unique<RuntimeReadyWorktreeFixture>();
        fixture->Repository = fixture->Git.Temporary.Path() /
                              (L"runtime checkpoint repository " +
                               std::to_wstring(seed));
        fixture->RuntimeRoot = fixture->Git.Temporary.Path() /
                               (L"runtime checkpoint data " +
                                std::to_wstring(seed));
        VERIFY_IS_TRUE(fixture->Git.Initialize(fixture->Repository));
        VERIFY_IS_TRUE(fixture->Git.Run(
                                  fixture->Repository,
                                  { L"config", L"user.name", L"Runtime Checkpoint Test" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture->Git.Run(
                                  fixture->Repository,
                                  { L"config", L"user.email", L"runtime-checkpoint@example.invalid" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture->Git.Run(
                                  fixture->Repository,
                                  { L"config", L"commit.gpgSign", L"false" })
                           .Succeeded());
        VERIFY_IS_TRUE(std::filesystem::create_directory(fixture->RuntimeRoot));
        const auto layout = PrepareGitWorktreeLayout(
            fixture->RuntimeRoot,
            MakeId<TaskId>(seed));
        VERIFY_IS_TRUE(layout.Succeeded());
        fixture->ManagedWorktree = layout.Layout->WorktreePath;
        GitRepositoryInspector inspector{ fixture->Git.GitExecutable };
        const auto repository = inspector.Inspect(
            fixture->Repository,
            fixture->Git.Cancellation.Get());
        VERIFY_IS_TRUE(repository.Succeeded());

        fixture->Task = MakeCreateTaskCommand(seed, seed, 1, 1000);
        fixture->Task.ExecutionContext.Mode = ExecutionMode::Worktree;
        fixture->Task.ExecutionContext.SourceRoot = Utf8Path(
            repository.Inspection->WorktreeIdentity.CanonicalPath());
        fixture->Task.ExecutionContext.RepositoryCommonDirectory = Utf8Path(
            repository.Inspection->CommonDirectoryIdentity.CanonicalPath());
        fixture->Task.ExecutionContext.WorktreePath =
            layout.Layout->WorktreePathUtf8;
        fixture->Task.ExecutionContext.RuntimeRoot =
            layout.Layout->WorktreePathUtf8;
        fixture->Task.ExecutionContext.Branch = layout.Layout->BranchRef;
        fixture->Run = MakeCreateRunCommand(
            fixture->Task,
            seed,
            2,
            1,
            1100);
        fixture->Store = std::make_shared<SqliteTaskStore>(
            fixture->Database.Path());
        VERIFY_IS_TRUE(fixture->Store->CreateTask(fixture->Task).Succeeded());
        VERIFY_IS_TRUE(fixture->Store->CreateTaskRun(fixture->Run).Succeeded());
        auto initialize = MakeInitializeGitWorktreeCommand(
            fixture->Task,
            3,
            2,
            1200);
        initialize.Worktree.RepositoryIdentityDigest =
            repository.Inspection->CommonDirectoryIdentity.Digest();
        initialize.Worktree.SourceHead = repository.Inspection->Head;
        initialize.Worktree.SourceReference = repository.Inspection->BranchRef;
        initialize.Worktree.OwnershipNonce =
            MakeId<CommandId>(90000 + seed).Value();
        VERIFY_IS_TRUE(
            fixture->Store->InitializeTaskGitWorktree(initialize).Succeeded());

        RuntimeGitWorktreeSetupService setup{
            fixture->Store,
            fixture->Git.GitExecutable,
            fixture->RuntimeRoot
        };
        for (uint64_t phase = 0; phase < 5; ++phase)
        {
            const auto result = setup.Step(
                fixture->Task.Task.Id,
                SetupContext(seed * 10 + phase, 1300 + phase * 100),
                fixture->Git.Cancellation.Get());
            VERIFY_IS_TRUE(result.Succeeded());
        }
        VERIFY_IS_TRUE(
            fixture->Worktree().State == TaskGitWorktreeState::Ready);
        return fixture;
    }

    inline void TransitionRunToTerminal(
        RuntimeReadyWorktreeFixture& fixture,
        const TaskRunState outcome,
        const uint64_t seed,
        const int64_t timestamp)
    {
        uint64_t offset{};
        for (const auto state : {
                 TaskRunState::Preparing,
                 TaskRunState::Starting,
                 TaskRunState::Running,
                 outcome,
             })
        {
            const auto aggregate = fixture.Aggregate();
            auto command = MakeRunStateCommand(
                fixture.Run,
                seed + offset,
                aggregate.Task.Version,
                timestamp + static_cast<int64_t>(offset) * 100,
                state,
                state == TaskRunState::Completed ? "completed" : std::string{},
                state == outcome && state != TaskRunState::Completed ?
                    "test-terminal-outcome" : std::string{});
            VERIFY_IS_TRUE(fixture.Store->ChangeTaskRunState(command).Succeeded());
            ++offset;
        }
    }

    inline void ExecuteDatabaseSql(
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
