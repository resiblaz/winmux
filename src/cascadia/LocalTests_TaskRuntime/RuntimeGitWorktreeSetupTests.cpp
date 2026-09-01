// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeGitWorktreeSetup.h"

#include "GitRepositoryInspector.h"
#include "GitSourceCaptureTestSupport.h"
#include "GitWorktreeLayout.h"
#include "GitWorktreeTestSupport.h"
#include "TaskCoreStoreTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskCoreLocalTests::TestFixtures;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        struct SetupFixture
        {
            GitFixture Git;
            TemporaryDatabasePath Database;
            std::filesystem::path Repository;
            std::filesystem::path RuntimeRoot;
            CreateTaskCommand Task;
            std::shared_ptr<SqliteTaskStore> Store;
        };

        RuntimeGitWorktreeSetupContext _context(
            const uint64_t seed,
            const int64_t timestamp)
        {
            RuntimeGitWorktreeSetupContext context;
            context.Command = MakeId<CommandId>(7000 + seed);
            context.Event = MakeId<TaskEventId>(8000 + seed);
            context.UnixMilliseconds = timestamp;
            return context;
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

        void _injectAdvanceFailure(const std::filesystem::path& path)
        {
            _executeSql(path, R"SQL(
CREATE TRIGGER fail_runtime_git_setup_event
BEFORE INSERT ON task_event
WHEN NEW.type='TaskGitWorktreeAdvanced'
BEGIN SELECT RAISE(ABORT, 'injected runtime git setup event failure'); END;
)SQL");
        }

        void _clearAdvanceFailure(const std::filesystem::path& path)
        {
            _executeSql(path, "DROP TRIGGER fail_runtime_git_setup_event;");
        }

        std::unique_ptr<SetupFixture> _setup(const uint64_t seed)
        {
            auto fixture = std::make_unique<SetupFixture>();
            fixture->Repository = fixture->Git.Temporary.Path() / L"setup repository";
            fixture->RuntimeRoot = fixture->Git.Temporary.Path() / L"runtime data";
            VERIFY_IS_TRUE(fixture->Git.Initialize(fixture->Repository));
            VERIFY_IS_TRUE(fixture->Git.Run(
                                      fixture->Repository,
                                      { L"config", L"user.name", L"Runtime Setup Test" })
                               .Succeeded());
            VERIFY_IS_TRUE(fixture->Git.Run(
                                      fixture->Repository,
                                      { L"config", L"user.email", L"runtime-setup@example.invalid" })
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

            fixture->Store = std::make_shared<SqliteTaskStore>(
                fixture->Database.Path());
            VERIFY_IS_TRUE(fixture->Store->CreateTask(fixture->Task).Succeeded());
            VERIFY_IS_TRUE(fixture->Store->CreateTaskRun(
                                      MakeCreateRunCommand(
                                          fixture->Task,
                                          1,
                                          2,
                                          1,
                                          1100))
                               .Succeeded());
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
                "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
            VERIFY_IS_TRUE(fixture->Store->InitializeTaskGitWorktree(initialize).Succeeded());
            return fixture;
        }

        TaskGitWorktreeRecord _worktree(const SetupFixture& fixture)
        {
            const auto aggregate = fixture.Store->LoadTask(fixture.Task.Task.Id);
            VERIFY_IS_TRUE(aggregate.has_value());
            VERIFY_IS_TRUE(aggregate->GitWorktree.has_value());
            return *aggregate->GitWorktree;
        }
    }

    class RuntimeGitWorktreeSetupTests
    {
        TEST_CLASS(RuntimeGitWorktreeSetupTests);

        TEST_METHOD(RecoversEveryExternalPhaseAfterStoreRollback);
        TEST_METHOD(PersistsCaptureCancellationAndRequiresExplicitRetry);
    };

    void RuntimeGitWorktreeSetupTests::RecoversEveryExternalPhaseAfterStoreRollback()
    {
        auto fixtureOwner = _setup(101);
        auto& fixture = *fixtureOwner;
        GitFixture::Write(fixture.Repository / L"tracked.txt", "runtime edit\n");
        GitFixture::Write(fixture.Repository / L"new.txt", "runtime new\n");
        RuntimeGitWorktreeSetupService setup{
            fixture.Store,
            fixture.Git.GitExecutable,
            fixture.RuntimeRoot
        };
        VERIFY_IS_TRUE(setup.Valid());

        auto result = setup.Step(
            fixture.Task.Task.Id,
            _context(1, 1300),
            fixture.Git.Cancellation.Get());
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            _worktree(fixture).State == TaskGitWorktreeState::CapturingSource);

        uint64_t seed{ 2 };
        int64_t timestamp{ 1400 };
        const std::array phases{
            std::pair{
                TaskGitWorktreeState::CapturingSource,
                TaskGitWorktreeState::Provisioning },
            std::pair{
                TaskGitWorktreeState::Provisioning,
                TaskGitWorktreeState::RestoringSource },
            std::pair{
                TaskGitWorktreeState::RestoringSource,
                TaskGitWorktreeState::CreatingBaseline },
            std::pair{
                TaskGitWorktreeState::CreatingBaseline,
                TaskGitWorktreeState::Ready },
        };
        for (const auto [current, next] : phases)
        {
            VERIFY_IS_TRUE(_worktree(fixture).State == current);
            _injectAdvanceFailure(fixture.Database.Path());
            result = setup.Step(
                fixture.Task.Task.Id,
                _context(seed++, timestamp++),
                fixture.Git.Cancellation.Get());
            VERIFY_IS_TRUE(
                result.Status == RuntimeGitWorktreeSetupStatus::StoreFailure);
            VERIFY_IS_TRUE(_worktree(fixture).State == current);
            _clearAdvanceFailure(fixture.Database.Path());

            result = setup.Step(
                fixture.Task.Task.Id,
                _context(seed++, timestamp++),
                fixture.Git.Cancellation.Get());
            VERIFY_IS_TRUE(result.Succeeded());
            VERIFY_IS_TRUE(_worktree(fixture).State == next);
        }

        const auto ready = _worktree(fixture);
        VERIFY_IS_TRUE(ready.State == TaskGitWorktreeState::Ready);
        VERIFY_IS_FALSE(ready.SnapshotDigest.empty());
        VERIFY_IS_FALSE(ready.BaselineCommit.empty());
        VERIFY_IS_FALSE(ready.HasUndeliveredWork);
        VERIFY_IS_TRUE(ready.PreservedPath.empty());
        VERIFY_ARE_EQUAL(
            std::string{ "runtime edit\n" },
            ReadBinaryFile(
                std::filesystem::path{ std::wstring{
                    ready.WorktreePath.begin(),
                    ready.WorktreePath.end() } } /
                L"tracked.txt"));

        result = setup.Step(
            fixture.Task.Task.Id,
            _context(seed, timestamp),
            fixture.Git.Cancellation.Get());
        VERIFY_IS_TRUE(result.Status == RuntimeGitWorktreeSetupStatus::Ready);
    }

    void RuntimeGitWorktreeSetupTests::PersistsCaptureCancellationAndRequiresExplicitRetry()
    {
        auto fixtureOwner = _setup(102);
        auto& fixture = *fixtureOwner;
        RuntimeGitWorktreeSetupService setup{
            fixture.Store,
            fixture.Git.GitExecutable,
            fixture.RuntimeRoot
        };
        VERIFY_IS_TRUE(setup.Step(
                           fixture.Task.Task.Id,
                           _context(20, 1300),
                           fixture.Git.Cancellation.Get())
                           .Succeeded());
        const auto cancellation = MakeEvent(true);
        VERIFY_IS_TRUE(static_cast<bool>(cancellation));
        const auto failed = setup.Step(
            fixture.Task.Task.Id,
            _context(21, 1400),
            cancellation.Get());
        VERIFY_IS_TRUE(
            failed.Status == RuntimeGitWorktreeSetupStatus::OperationFailed);
        VERIFY_IS_TRUE(failed.FailurePersisted);
        VERIFY_ARE_EQUAL(
            std::string{ "git-source-capture-failed" },
            failed.FailureReasonCode);
        const auto record = _worktree(fixture);
        VERIFY_IS_TRUE(record.State == TaskGitWorktreeState::Failed);
        VERIFY_ARE_EQUAL(
            std::string{ "git-source-capture-failed" },
            record.FailureReasonCode);
        VERIFY_IS_TRUE(record.SnapshotDigest.empty());
        VERIFY_IS_FALSE(record.HasUndeliveredWork);

        const auto retry = setup.Step(
            fixture.Task.Task.Id,
            _context(22, 1500),
            fixture.Git.Cancellation.Get());
        VERIFY_IS_TRUE(
            retry.Status == RuntimeGitWorktreeSetupStatus::UnsupportedState);
    }
}
