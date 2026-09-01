// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceCaptureTestSupport.h"
#include "GitSourceCommandOutput.h"
#include "GitWorktreeProvision.h"
#include "GitWorktreeTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    class GitWorktreeProvisionTests
    {
        TEST_CLASS(GitWorktreeProvisionTests);

        TEST_METHOD(CreatesDeterministicBranchWithoutMutatingSourceState);
        TEST_METHOD(RecoversDetachedAndAlreadyAttachedWorktrees);
        TEST_METHOD(PreservesDetachedWorktreeWhenBranchAttachmentFails);
        TEST_METHOD(RejectsCollisionAndCancellationBeforeCreation);
    };

    void GitWorktreeProvisionTests::CreatesDeterministicBranchWithoutMutatingSourceState()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"provision",
            TaskId{ "91000000-0000-4000-8000-000000000001" });
        PersistTaskWorktreeEvidence(prepared);
        const auto checkoutHook =
            prepared.Repository / L".git" / L"hooks" / L"post-checkout";
        GitFixture::Write(checkoutHook, "#!/bin/sh\nexit 97\n");
        std::filesystem::permissions(
            checkoutHook,
            std::filesystem::perms::owner_exec |
                std::filesystem::perms::group_exec |
                std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add);
        const auto hookProof = fixture.Run(
            prepared.Repository,
            {
                L"hook",
                L"run",
                L"post-checkout",
                L"--",
                L"old",
                L"new",
                L"1",
            });
        VERIFY_IS_TRUE(hookProof.Status == GitCommandStatus::Completed);
        VERIFY_ARE_EQUAL(DWORD{ 97 }, hookProof.ExitCode);
        const auto before = ObserveGitSourceState(
            fixture,
            prepared.Repository,
            { L"tracked.txt" });

        const auto result = ProvisionTaskWorktree(
            fixture,
            prepared,
            GitWorktreePreflightMode::RequireUnoccupied);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            result.InitialDisposition ==
            GitWorktreePreflightDisposition::CreateNew);
        VERIFY_IS_TRUE(result.WorktreeCreated);
        VERIFY_IS_TRUE(result.BranchCreated);
        VERIFY_IS_FALSE(result.Preserved);
        VERIFY_IS_TRUE(std::filesystem::is_directory(
            prepared.Layout.WorktreePath));
        VERIFY_ARE_EQUAL(
            prepared.Layout.BranchRef,
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"symbolic-ref", L"HEAD" })));
        VERIFY_ARE_EQUAL(
            prepared.Journal.SourceHead,
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"rev-parse", L"--verify", L"HEAD" })));

        const auto after = ObserveGitSourceState(
            fixture,
            prepared.Repository,
            { L"tracked.txt" });
        VERIFY_IS_TRUE(SameGitSourceState(before, after));
        const auto recovery = InspectTaskWorktreePreflight(
            fixture,
            prepared,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(recovery.Succeeded());
        VERIFY_IS_TRUE(
            recovery.Disposition ==
            GitWorktreePreflightDisposition::ResumeBranch);
    }

    void GitWorktreeProvisionTests::RecoversDetachedAndAlreadyAttachedWorktrees()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"recover provision",
            TaskId{ "92000000-0000-4000-8000-000000000002" });
        PersistTaskWorktreeEvidence(prepared);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  {
                                      L"-c",
                                      L"core.hooksPath=NUL",
                                      L"worktree",
                                      L"add",
                                      L"--detach",
                                      prepared.Layout.WorktreePath.native(),
                                      details::WidenGitSourceAscii(
                                          prepared.Journal.SourceHead),
                                  })
                           .Succeeded());

        auto result = ProvisionTaskWorktree(
            fixture,
            prepared,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            result.InitialDisposition ==
            GitWorktreePreflightDisposition::ResumeDetached);
        VERIFY_IS_FALSE(result.WorktreeCreated);
        VERIFY_IS_TRUE(result.BranchCreated);

        result = ProvisionTaskWorktree(
            fixture,
            prepared,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            result.InitialDisposition ==
            GitWorktreePreflightDisposition::ResumeBranch);
        VERIFY_IS_FALSE(result.WorktreeCreated);
        VERIFY_IS_FALSE(result.BranchCreated);
    }

    void GitWorktreeProvisionTests::PreservesDetachedWorktreeWhenBranchAttachmentFails()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"failed attach",
            TaskId{ "93000000-0000-4000-8000-000000000003" });
        PersistTaskWorktreeEvidence(prepared);
        GitCommandRunner runner;
        GitWorktreeProvisioner failing{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                if (std::ranges::find(request.Arguments, L"switch") !=
                    request.Arguments.end())
                {
                    GitCommandResult result;
                    result.Status = GitCommandStatus::Completed;
                    result.ExitCode = 1;
                    result.Stderr = "injected branch attachment failure";
                    return result;
                }
                return runner.Run(request);
            }
        };
        const auto failed = failing.Provision(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get(),
            GitWorktreePreflightMode::RequireUnoccupied);
        VERIFY_IS_TRUE(failed.Status == GitWorktreeProvisionStatus::GitFailure);
        VERIFY_IS_TRUE(failed.WorktreeCreated);
        VERIFY_IS_FALSE(failed.BranchCreated);
        VERIFY_IS_TRUE(failed.Preserved);
        VERIFY_ARE_EQUAL(
            prepared.Layout.WorktreePathUtf8,
            failed.PreservedPath);
        const auto detached = InspectTaskWorktreePreflight(
            fixture,
            prepared,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(detached.Succeeded());
        VERIFY_IS_TRUE(
            detached.Disposition ==
            GitWorktreePreflightDisposition::ResumeDetached);

        const auto recovered = ProvisionTaskWorktree(
            fixture,
            prepared,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(
            recovered.InitialDisposition ==
            GitWorktreePreflightDisposition::ResumeDetached);
        VERIFY_IS_TRUE(recovered.BranchCreated);
    }

    void GitWorktreeProvisionTests::RejectsCollisionAndCancellationBeforeCreation()
    {
        GitFixture fixture;
        auto collision = PrepareTaskWorktree(
            fixture,
            L"provision collision",
            TaskId{ "94000000-0000-4000-8000-000000000004" });
        PersistTaskWorktreeEvidence(collision);
        VERIFY_IS_TRUE(fixture.Run(
                                  collision.Repository,
                                  {
                                      L"branch",
                                      details::WidenGitSourceAscii(
                                          collision.Layout.BranchName),
                                      details::WidenGitSourceAscii(
                                          collision.Journal.SourceHead),
                                  })
                           .Succeeded());
        const auto rejected = ProvisionTaskWorktree(
            fixture,
            collision,
            GitWorktreePreflightMode::RequireUnoccupied);
        VERIFY_IS_TRUE(
            rejected.Status == GitWorktreeProvisionStatus::PreflightRejected);
        VERIFY_IS_TRUE(
            rejected.PreflightStatus ==
            GitWorktreePreflightStatus::BranchCollision);
        VERIFY_IS_FALSE(rejected.Preserved);
        VERIFY_IS_FALSE(std::filesystem::exists(
            collision.Layout.WorktreePath));

        auto cancelledTask = PrepareTaskWorktree(
            fixture,
            L"cancelled provision",
            TaskId{ "94000000-0000-4000-8000-000000000005" });
        const auto missingEvidence = ProvisionTaskWorktree(
            fixture,
            cancelledTask,
            GitWorktreePreflightMode::RequireUnoccupied);
        VERIFY_IS_TRUE(
            missingEvidence.Status ==
            GitWorktreeProvisionStatus::PreflightRejected);
        VERIFY_IS_FALSE(std::filesystem::exists(
            cancelledTask.Layout.WorktreePath));
        PersistTaskWorktreeEvidence(cancelledTask);
        const auto cancellation = MakeEvent(true);
        VERIFY_IS_TRUE(static_cast<bool>(cancellation));
        const auto cancelled = ProvisionTaskWorktree(
            fixture,
            cancelledTask,
            GitWorktreePreflightMode::RequireUnoccupied,
            cancellation.Get());
        VERIFY_IS_TRUE(
            cancelled.Status == GitWorktreeProvisionStatus::Cancelled);
        VERIFY_IS_FALSE(cancelled.Preserved);
        VERIFY_IS_FALSE(std::filesystem::exists(
            cancelledTask.Layout.WorktreePath));
    }
}
