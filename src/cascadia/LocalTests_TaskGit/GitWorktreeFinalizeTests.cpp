// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceCaptureTestSupport.h"
#include "GitSourceCommandOutput.h"
#include "GitWorktreeFinalize.h"
#include "GitWorktreeTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    namespace
    {
        GitWorktreeFinalizeResult _finalize(
            const GitFixture& fixture,
            const PreparedTaskWorktree& prepared,
            const bool expectedUndelivered = false)
        {
            GitWorktreeFinalizer finalizer{ fixture.GitExecutable };
            return finalizer.Finalize(
                prepared.Layout,
                prepared.Marker,
                prepared.Journal,
                prepared.Repository,
                expectedUndelivered,
                fixture.Cancellation.Get());
        }

        GitCommandResult _branch(
            const GitFixture& fixture,
            const PreparedTaskWorktree& prepared)
        {
            return fixture.Run(
                prepared.Repository,
                {
                    L"rev-parse",
                    L"--verify",
                    details::WidenGitSourceAscii(prepared.Layout.BranchRef),
                });
        }

        void _agentCommit(
            const GitFixture& fixture,
            const PreparedTaskWorktree& prepared)
        {
            VERIFY_IS_TRUE(fixture.Run(
                                      prepared.Layout.WorktreePath,
                                      { L"add", L"-A", L"--" })
                               .Succeeded());
            VERIFY_IS_TRUE(fixture.Run(
                                      prepared.Layout.WorktreePath,
                                      {
                                          L"-c",
                                          L"user.name=Finalize Agent",
                                          L"-c",
                                          L"user.email=finalize-agent@example.invalid",
                                          L"-c",
                                          L"commit.gpgSign=false",
                                          L"-c",
                                          L"core.hooksPath=NUL",
                                          L"commit",
                                          L"--no-verify",
                                          L"-m",
                                          L"finalize agent commit",
                                      })
                               .Succeeded());
        }
    }

    class GitWorktreeFinalizeTests
    {
        TEST_CLASS(GitWorktreeFinalizeTests);

        TEST_METHOD(RemovesBaselineEqualWorktreeAndBranchIdempotently);
        TEST_METHOD(OccupiedDirectoryIsPreservedAndCleanupCanRetry);
        TEST_METHOD(RecoversEmptyDirectoryAfterGitRemovedRegistration);
        TEST_METHOD(CheckpointsRemainderAndRetainsProducedBranchAndAgentCommit);
        TEST_METHOD(PreservesIgnoredAndSigningFailuresForRetry);
        TEST_METHOD(RejectsOwnershipMismatchWithoutRemovingAnything);
        TEST_METHOD(RecoversPostCommitObservationWithoutDuplicateCommit);
        TEST_METHOD(ResumesSidecarCleanupAfterWorktreeAndBranchRemoval);
    };

    void GitWorktreeFinalizeTests::OccupiedDirectoryIsPreservedAndCleanupCanRetry()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(fixture, L"occupied finalize",
            TaskId{ "e1000000-0000-4000-8000-000000000002" });
        PrepareReadyTaskWorktree(fixture, prepared);
        const auto occupied = CreateFileW(prepared.Layout.WorktreePath.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        VERIFY_IS_TRUE(occupied != INVALID_HANDLE_VALUE);
        const auto result = _finalize(fixture, prepared);
        const auto registrationRetained = std::filesystem::exists(prepared.Layout.WorktreePath / L".git");
        CloseHandle(occupied);
        VERIFY_IS_FALSE(result.Succeeded());
        VERIFY_IS_TRUE(result.Preserved);
        VERIFY_IS_TRUE(registrationRetained);
        VERIFY_IS_TRUE(_finalize(fixture, prepared).Succeeded());
    }

    void GitWorktreeFinalizeTests::RecoversEmptyDirectoryAfterGitRemovedRegistration()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(fixture, L"partial finalize",
            TaskId{ "e1000000-0000-4000-8000-000000000003" });
        PrepareReadyTaskWorktree(fixture, prepared);
        VERIFY_IS_TRUE(fixture.Run(prepared.Repository,
            { L"worktree", L"remove", L"--", prepared.Layout.WorktreePath.native() }).Succeeded());
        VERIFY_IS_TRUE(std::filesystem::create_directory(prepared.Layout.WorktreePath));
        const auto result = _finalize(fixture, prepared);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(result.WorktreeRemoved);
        VERIFY_IS_FALSE(std::filesystem::exists(prepared.Layout.WorktreePath));
    }

    void GitWorktreeFinalizeTests::RemovesBaselineEqualWorktreeAndBranchIdempotently()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"empty finalize",
            TaskId{ "e1000000-0000-4000-8000-000000000001" });
        PrepareReadyTaskWorktree(fixture, prepared);
        const auto result = _finalize(fixture, prepared);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_FALSE(result.ProducedWork);
        VERIFY_IS_TRUE(result.WorktreeRemoved);
        VERIFY_IS_TRUE(result.BranchDeleted);
        VERIFY_IS_FALSE(result.BranchRetained);
        VERIFY_IS_TRUE(result.SidecarsRemoved);
        VERIFY_IS_FALSE(std::filesystem::exists(prepared.Layout.WorktreePath));
        VERIFY_IS_FALSE(std::filesystem::exists(prepared.Layout.MarkerPath));
        VERIFY_IS_FALSE(std::filesystem::exists(prepared.Layout.SnapshotPath));
        VERIFY_IS_TRUE(_branch(fixture, prepared).ExitCode != 0);
        VERIFY_IS_TRUE(std::filesystem::is_directory(prepared.Repository));

        const auto recovered = _finalize(fixture, prepared);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(
            recovered.Status == GitWorktreeFinalizeStatus::AlreadyFinalized);
        VERIFY_IS_TRUE(recovered.BranchDeleted);
    }

    void GitWorktreeFinalizeTests::CheckpointsRemainderAndRetainsProducedBranchAndAgentCommit()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"produced finalize",
            TaskId{ "e2000000-0000-4000-8000-000000000002" });
        PrepareReadyTaskWorktree(fixture, prepared);
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "agent commit\n");
        _agentCommit(fixture, prepared);
        const auto agentCommit = SingleLine(RunCompleted(
            fixture,
            prepared.Layout.WorktreePath,
            { L"rev-parse", L"HEAD" }));
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "final remainder\n");
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"result.txt",
            "result\n");

        const auto result = _finalize(fixture, prepared, true);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(result.ProducedWork);
        VERIFY_IS_TRUE(result.BranchRetained);
        VERIFY_IS_FALSE(result.BranchDeleted);
        VERIFY_IS_TRUE(result.CheckpointCreated);
        VERIFY_IS_FALSE(result.CheckpointCommit.empty());
        VERIFY_ARE_EQUAL(result.FinalCommit, SingleLine(_branch(fixture, prepared)));
        VERIFY_ARE_EQUAL(
            agentCommit,
            SingleLine(RunCompleted(
                fixture,
                prepared.Repository,
                {
                    L"rev-parse",
                    details::WidenGitSourceAscii(result.FinalCommit) + L"^",
                })));
        VERIFY_ARE_EQUAL(
            std::string{ "finalize agent commit" },
            SingleLine(RunCompleted(
                fixture,
                prepared.Repository,
                {
                    L"show",
                    L"-s",
                    L"--format=%s",
                    details::WidenGitSourceAscii(agentCommit),
                })));
    }

    void GitWorktreeFinalizeTests::PreservesIgnoredAndSigningFailuresForRetry()
    {
        GitFixture fixture;
        auto ignored = PrepareTaskWorktree(
            fixture,
            L"ignored finalize",
            TaskId{ "e3000000-0000-4000-8000-000000000003" });
        GitFixture::Write(ignored.Repository / L".gitignore", "ignored.bin\n");
        VERIFY_IS_TRUE(CommitAll(fixture, ignored.Repository, L"ignore output"));
        RecaptureTaskWorktree(fixture, ignored);
        PrepareReadyTaskWorktree(fixture, ignored);
        GitFixture::Write(
            ignored.Layout.WorktreePath / L"ignored.bin",
            "must not delete\n");
        const auto ignoredFailure = _finalize(fixture, ignored);
        VERIFY_IS_TRUE(
            ignoredFailure.Status == GitWorktreeFinalizeStatus::TargetChanged);
        VERIFY_IS_TRUE(ignoredFailure.Preserved);
        VERIFY_IS_TRUE(std::filesystem::exists(ignored.Layout.WorktreePath));
        VERIFY_IS_TRUE(std::filesystem::exists(
            ignored.Layout.WorktreePath / L"ignored.bin"));

        auto signing = PrepareTaskWorktree(
            fixture,
            L"sign finalize",
            TaskId{ "e3100000-0000-4000-8000-000000000004" });
        PrepareReadyTaskWorktree(fixture, signing);
        VERIFY_IS_TRUE(fixture.Run(
                                  signing.Layout.WorktreePath,
                                  { L"config", L"user.name", L"Signing Finalize" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  signing.Layout.WorktreePath,
                                  { L"config", L"user.email", L"sign-finalize@example.invalid" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  signing.Layout.WorktreePath,
                                  { L"config", L"commit.gpgSign", L"true" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  signing.Layout.WorktreePath,
                                  { L"config", L"gpg.program", L"missing-task-gpg.exe" })
                           .Succeeded());
        GitFixture::Write(
            signing.Layout.WorktreePath / L"tracked.txt",
            "signed finalization\n");
        const auto signingFailure = _finalize(fixture, signing);
        VERIFY_IS_TRUE(
            signingFailure.Status == GitWorktreeFinalizeStatus::SigningFailure);
        VERIFY_IS_TRUE(signingFailure.Preserved);
        VERIFY_IS_TRUE(signingFailure.SigningConfigured);
        VERIFY_IS_TRUE(std::filesystem::exists(signing.Layout.WorktreePath));
        VERIFY_IS_TRUE(fixture.Run(
                                  signing.Layout.WorktreePath,
                                  { L"config", L"commit.gpgSign", L"false" })
                           .Succeeded());
        const auto recovered = _finalize(fixture, signing);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(recovered.BranchRetained);
    }

    void GitWorktreeFinalizeTests::RejectsOwnershipMismatchWithoutRemovingAnything()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"identity finalize",
            TaskId{ "e4000000-0000-4000-8000-000000000004" });
        PrepareReadyTaskWorktree(fixture, prepared);
        auto wrong = prepared.Marker;
        wrong.OwnershipNonce = "ffffffff-ffff-4fff-8fff-ffffffffffff";
        GitWorktreeFinalizer finalizer{ fixture.GitExecutable };
        const auto result = finalizer.Finalize(
            prepared.Layout,
            wrong,
            prepared.Journal,
            prepared.Repository,
            false,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(
            result.Status == GitWorktreeFinalizeStatus::EvidenceMismatch);
        VERIFY_IS_TRUE(result.Preserved);
        VERIFY_IS_TRUE(std::filesystem::exists(prepared.Layout.WorktreePath));
        VERIFY_IS_TRUE(_branch(fixture, prepared).Succeeded());
    }

    void GitWorktreeFinalizeTests::RecoversPostCommitObservationWithoutDuplicateCommit()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"commit recovery finalize",
            TaskId{ "e5000000-0000-4000-8000-000000000005" });
        PrepareReadyTaskWorktree(fixture, prepared);
        GitFixture::Write(
            prepared.Layout.WorktreePath / L"tracked.txt",
            "recover finalization\n");
        GitCommandRunner runner;
        bool committed{};
        bool failedObservation{};
        GitWorktreeFinalizer unreliable{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                const auto isCommit = std::ranges::find(
                                          request.Arguments,
                                          L"commit") != request.Arguments.end();
                auto result = runner.Run(request);
                committed |= isCommit && result.Succeeded();
                if (committed && !failedObservation &&
                    request.Arguments == std::vector<std::wstring>{
                                             L"rev-parse", L"--verify", L"HEAD" })
                {
                    failedObservation = true;
                    GitCommandResult failure;
                    failure.Status = GitCommandStatus::TimedOut;
                    failure.SystemError = WAIT_TIMEOUT;
                    failure.Error = "injected finalization observation failure";
                    return failure;
                }
                return result;
            }
        };
        const auto uncertain = unreliable.Finalize(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            prepared.Repository,
            false,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(uncertain.Status == GitWorktreeFinalizeStatus::TimedOut);
        VERIFY_IS_TRUE(uncertain.Preserved);
        VERIFY_IS_TRUE(std::filesystem::exists(prepared.Layout.WorktreePath));

        const auto recovered = _finalize(fixture, prepared);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(recovered.BranchRetained);
        VERIFY_IS_FALSE(recovered.CheckpointCreated);
        VERIFY_IS_FALSE(recovered.CheckpointCommit.empty());
        VERIFY_ARE_EQUAL(
            std::string{ "1" },
            SingleLine(RunCompleted(
                fixture,
                prepared.Repository,
                {
                    L"rev-list",
                    L"--count",
                    details::WidenGitSourceAscii(prepared.Marker.BaselineCommit) +
                        L".." +
                        details::WidenGitSourceAscii(recovered.FinalCommit),
                })));
    }

    void GitWorktreeFinalizeTests::ResumesSidecarCleanupAfterWorktreeAndBranchRemoval()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"sidecar recovery finalize",
            TaskId{ "e6000000-0000-4000-8000-000000000006" });
        PrepareReadyTaskWorktree(fixture, prepared);
        UniqueHandle markerHandle{ CreateFileW(
            prepared.Layout.MarkerPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr) };
        VERIFY_IS_TRUE(static_cast<bool>(markerHandle));
        const auto failed = _finalize(fixture, prepared);
        VERIFY_IS_TRUE(
            failed.Status == GitWorktreeFinalizeStatus::SidecarCleanupFailed);
        VERIFY_IS_TRUE(failed.WorktreeRemoved);
        VERIFY_IS_TRUE(failed.BranchDeleted);
        VERIFY_IS_FALSE(std::filesystem::exists(prepared.Layout.WorktreePath));
        VERIFY_IS_TRUE(_branch(fixture, prepared).ExitCode != 0);
        VERIFY_IS_TRUE(std::filesystem::exists(prepared.Layout.MarkerPath));
        markerHandle.Reset();

        const auto recovered = _finalize(fixture, prepared);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(
            recovered.Status == GitWorktreeFinalizeStatus::AlreadyFinalized);
        VERIFY_IS_TRUE(recovered.SidecarsRemoved);
        VERIFY_IS_FALSE(std::filesystem::exists(prepared.Layout.MarkerPath));
    }
}
