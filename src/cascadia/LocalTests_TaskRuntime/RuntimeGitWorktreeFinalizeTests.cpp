// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeGitWorktreeFinalize.h"
#include "RuntimeGitWorktreeFinalizeStore.h"
#include "RuntimeGitWorktreeTestSupport.h"

#include "GitSourceCommandOutput.h"
#include "GitSourceCaptureTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskCoreLocalTests::TestFixtures;
using namespace TaskGitLocalTests::TestSupport;
using namespace TaskRuntimeLocalTests::GitTestSupport;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        RuntimeGitWorktreeFinalizeContext _context(
            const uint64_t seed,
            const int64_t timestamp,
            const bool retry = false)
        {
            RuntimeGitWorktreeFinalizeContext context;
            context.Command = MakeId<CommandId>(140000 + seed);
            context.Event = MakeId<TaskEventId>(150000 + seed);
            context.UnixMilliseconds = timestamp;
            context.RetryPreserved = retry;
            return context;
        }

        RuntimeGitWorktreeFinalizeResult _step(
            RuntimeReadyWorktreeFixture& fixture,
            RuntimeGitWorktreeFinalizeService& service,
            const uint64_t seed,
            const int64_t timestamp,
            const bool retry = false)
        {
            return service.Step(
                fixture.Task.Task.Id,
                _context(seed, timestamp, retry),
                fixture.Git.Cancellation.Get());
        }

        GitCommandResult _branch(RuntimeReadyWorktreeFixture& fixture)
        {
            const auto worktree = fixture.Worktree();
            return fixture.Git.Run(
                fixture.Repository,
                {
                    L"rev-parse",
                    L"--verify",
                    Microsoft::Terminal::TaskGit::details::WidenGitSourceAscii(worktree.BranchRef),
                });
        }
    }

    class RuntimeGitWorktreeFinalizeTests
    {
        TEST_CLASS(RuntimeGitWorktreeFinalizeTests);

        TEST_METHOD(BlocksActiveRunThenFinalizesEmptyWorktreeWithoutChangingTaskStatus);
        TEST_METHOD(CheckpointsDirtyRemainderAndRetainsResultBranch);
        TEST_METHOD(PersistsSigningFailureAndRetriesPreservedWorktree);
        TEST_METHOD(RecoversAfterGitRemovalBeforeSqlitePublication);
        TEST_METHOD(ResumesSidecarFailureWithoutRepeatingGitRemoval);
        TEST_METHOD(PublicationRetriesAggregateVersionWithoutChangingWorktreeCas);
    };

    void RuntimeGitWorktreeFinalizeTests::PublicationRetriesAggregateVersionWithoutChangingWorktreeCas()
    {
        auto fixture = MakeReadyWorktree(506);
        TransitionRunToTerminal(*fixture, TaskRunState::Cancelled, 16000, 3000);
        const auto stale = fixture->Aggregate();
        VERIFY_IS_TRUE(fixture->Store->ChangeTaskManualHold(
            MakeManualHoldCommand(fixture->Task, 16020, stale.Task.Version, stale.Task.UpdatedAtUnixMilliseconds + 1, true)).Succeeded());
        const auto stored = Microsoft::Terminal::TaskRuntime::details::AdvanceRuntimeGitWorktreeFinalization(
            *fixture->Store, stale, _context(1, 4000), TaskGitWorktreeState::Finalizing, false, false);
        VERIFY_IS_TRUE(stored.Succeeded());
        const auto updated = fixture->Aggregate();
        VERIFY_ARE_EQUAL(stale.Task.Version + 2, updated.Task.Version);
        VERIFY_ARE_EQUAL(stale.GitWorktree->Revision + 1, updated.GitWorktree->Revision);
        VERIFY_IS_TRUE(updated.GitWorktree->State == TaskGitWorktreeState::Finalizing);
    }

    void RuntimeGitWorktreeFinalizeTests::BlocksActiveRunThenFinalizesEmptyWorktreeWithoutChangingTaskStatus()
    {
        auto fixture = MakeReadyWorktree(501);
        RuntimeGitWorktreeFinalizeService service{
            fixture->Store,
            fixture->Git.GitExecutable,
            fixture->RuntimeRoot
        };
        const auto blocked = _step(*fixture, service, 1, 3000);
        VERIFY_IS_TRUE(
            blocked.Status == RuntimeGitWorktreeFinalizeStatus::StoreFailure);
        VERIFY_IS_TRUE(blocked.StoreStatus == TaskStoreCommandStatus::ActiveRunExists);
        VERIFY_IS_TRUE(
            fixture->Worktree().State == TaskGitWorktreeState::Ready);

        TransitionRunToTerminal(*fixture, TaskRunState::Cancelled, 8000, 3100);
        const auto started = _step(*fixture, service, 2, 4000);
        VERIFY_IS_TRUE(started.Succeeded());
        VERIFY_IS_TRUE(
            fixture->Worktree().State == TaskGitWorktreeState::Finalizing);
        const auto completed = _step(*fixture, service, 3, 4100);
        VERIFY_IS_TRUE(completed.Succeeded());
        VERIFY_IS_TRUE(completed.BranchDeleted);
        VERIFY_IS_TRUE(completed.WorktreeRemoved);
        const auto aggregate = fixture->Aggregate();
        VERIFY_IS_TRUE(
            aggregate.GitWorktree->State == TaskGitWorktreeState::Finalized);
        VERIFY_IS_TRUE(aggregate.GitWorktree->CleanupEligible);
        VERIFY_IS_FALSE(aggregate.GitWorktree->HasUndeliveredWork);
        VERIFY_IS_TRUE(aggregate.Task.Status == TaskStatus::Todo);
        VERIFY_IS_FALSE(std::filesystem::exists(fixture->ManagedWorktree));
        VERIFY_IS_TRUE(_branch(*fixture).ExitCode != 0);
    }

    void RuntimeGitWorktreeFinalizeTests::CheckpointsDirtyRemainderAndRetainsResultBranch()
    {
        auto fixture = MakeReadyWorktree(502);
        RuntimeGitWorktreeFinalizeService service{
            fixture->Store,
            fixture->Git.GitExecutable,
            fixture->RuntimeRoot
        };
        TransitionRunToTerminal(*fixture, TaskRunState::Completed, 9000, 3000);
        GitFixture::Write(
            fixture->ManagedWorktree / L"result.txt",
            "late final output\n");
        VERIFY_IS_TRUE(_step(*fixture, service, 1, 4000).Succeeded());
        const auto completed = _step(*fixture, service, 2, 4100);
        VERIFY_IS_TRUE(completed.Succeeded());
        VERIFY_IS_TRUE(completed.ProducedWork);
        VERIFY_IS_TRUE(completed.BranchRetained);
        VERIFY_IS_FALSE(completed.BranchDeleted);
        VERIFY_IS_FALSE(completed.FinalCommit.empty());
        VERIFY_ARE_EQUAL(completed.FinalCommit, SingleLine(_branch(*fixture)));
        VERIFY_IS_FALSE(std::filesystem::exists(fixture->ManagedWorktree));
    }

    void RuntimeGitWorktreeFinalizeTests::PersistsSigningFailureAndRetriesPreservedWorktree()
    {
        auto fixture = MakeReadyWorktree(503);
        RuntimeGitWorktreeFinalizeService service{
            fixture->Store,
            fixture->Git.GitExecutable,
            fixture->RuntimeRoot
        };
        TransitionRunToTerminal(*fixture, TaskRunState::Failed, 10000, 3000);
        VERIFY_IS_TRUE(_step(*fixture, service, 1, 4000).Succeeded());
        VERIFY_IS_TRUE(fixture->Git.Run(
                                  fixture->ManagedWorktree,
                                  { L"config", L"commit.gpgSign", L"true" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture->Git.Run(
                                  fixture->ManagedWorktree,
                                  { L"config", L"gpg.program", L"missing-task-gpg.exe" })
                           .Succeeded());
        GitFixture::Write(
            fixture->ManagedWorktree / L"tracked.txt",
            "signing retry\n");
        const auto failed = _step(*fixture, service, 2, 4100);
        VERIFY_IS_TRUE(
            failed.Status == RuntimeGitWorktreeFinalizeStatus::OperationFailed);
        VERIFY_IS_TRUE(failed.FailurePersisted);
        VERIFY_ARE_EQUAL(
            std::string{ "git-finalize-signing-failed" },
            failed.ReasonCode);
        VERIFY_ARE_EQUAL(fixture->Worktree().WorktreePath, failed.PreservedPath);
        VERIFY_IS_TRUE(
            fixture->Worktree().State == TaskGitWorktreeState::Preserved);
        VERIFY_IS_TRUE(std::filesystem::exists(fixture->ManagedWorktree));

        VERIFY_IS_TRUE(fixture->Git.Run(
                                  fixture->ManagedWorktree,
                                  { L"config", L"commit.gpgSign", L"false" })
                           .Succeeded());
        VERIFY_IS_TRUE(_step(*fixture, service, 3, 4200, true).Succeeded());
        const auto recovered = _step(*fixture, service, 4, 4300);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(recovered.BranchRetained);
        VERIFY_IS_TRUE(
            fixture->Worktree().State == TaskGitWorktreeState::Finalized);
    }

    void RuntimeGitWorktreeFinalizeTests::RecoversAfterGitRemovalBeforeSqlitePublication()
    {
        auto fixture = MakeReadyWorktree(504);
        RuntimeGitWorktreeFinalizeService service{
            fixture->Store,
            fixture->Git.GitExecutable,
            fixture->RuntimeRoot
        };
        TransitionRunToTerminal(*fixture, TaskRunState::Interrupted, 11000, 3000);
        VERIFY_IS_TRUE(_step(*fixture, service, 1, 4000).Succeeded());
        ExecuteDatabaseSql(fixture->Database.Path(), R"SQL(
CREATE TRIGGER fail_finalize_publish
BEFORE INSERT ON task_event
WHEN NEW.type='TaskGitWorktreeFinalizationAdvanced'
BEGIN SELECT RAISE(ABORT, 'injected finalization publish failure'); END;
)SQL");
        const auto failed = _step(*fixture, service, 2, 4100);
        VERIFY_IS_TRUE(
            failed.Status == RuntimeGitWorktreeFinalizeStatus::StoreFailure);
        VERIFY_IS_TRUE(failed.WorktreeRemoved);
        VERIFY_IS_TRUE(failed.BranchDeleted);
        VERIFY_IS_FALSE(std::filesystem::exists(fixture->ManagedWorktree));
        VERIFY_IS_TRUE(
            fixture->Worktree().State == TaskGitWorktreeState::Finalizing);
        ExecuteDatabaseSql(
            fixture->Database.Path(),
            "DROP TRIGGER fail_finalize_publish;");
        const auto recovered = _step(*fixture, service, 3, 4200);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(
            fixture->Worktree().State == TaskGitWorktreeState::Finalized);
    }

    void RuntimeGitWorktreeFinalizeTests::ResumesSidecarFailureWithoutRepeatingGitRemoval()
    {
        auto fixture = MakeReadyWorktree(505);
        RuntimeGitWorktreeFinalizeService service{
            fixture->Store,
            fixture->Git.GitExecutable,
            fixture->RuntimeRoot
        };
        TransitionRunToTerminal(*fixture, TaskRunState::Cancelled, 12000, 3000);
        VERIFY_IS_TRUE(_step(*fixture, service, 1, 4000).Succeeded());
        const auto layout = PrepareGitWorktreeLayout(
            fixture->RuntimeRoot,
            fixture->Task.Task.Id);
        VERIFY_IS_TRUE(layout.Succeeded());
        UniqueHandle markerHandle{ CreateFileW(
            layout.Layout->MarkerPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr) };
        VERIFY_IS_TRUE(static_cast<bool>(markerHandle));
        const auto failed = _step(*fixture, service, 2, 4100);
        VERIFY_IS_TRUE(
            failed.Status == RuntimeGitWorktreeFinalizeStatus::OperationFailed);
        VERIFY_IS_TRUE(failed.WorktreeRemoved);
        VERIFY_IS_TRUE(failed.BranchDeleted);
        VERIFY_IS_FALSE(failed.FailurePersisted);
        VERIFY_IS_TRUE(
            fixture->Worktree().State == TaskGitWorktreeState::Finalizing);
        markerHandle.Reset();
        const auto recovered = _step(*fixture, service, 3, 4200);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(
            fixture->Worktree().State == TaskGitWorktreeState::Finalized);
    }
}
