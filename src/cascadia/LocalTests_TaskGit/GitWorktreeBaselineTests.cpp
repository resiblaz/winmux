// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceCaptureTestSupport.h"
#include "GitSourceCommandOutput.h"
#include "GitWorktreeBaseline.h"
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
        GitWorktreeBaselineResult _baseline(
            const GitFixture& fixture,
            const PreparedTaskWorktree& prepared)
        {
            GitWorktreeBaselineCreator creator{ fixture.GitExecutable };
            return creator.Create(
                prepared.Layout,
                prepared.Marker,
                prepared.Journal,
                fixture.Cancellation.Get());
        }

        void _prepareForBaseline(
            const GitFixture& fixture,
            PreparedTaskWorktree& prepared)
        {
            PersistTaskWorktreeEvidence(prepared);
            VERIFY_IS_TRUE(ProvisionTaskWorktree(
                               fixture,
                               prepared,
                               GitWorktreePreflightMode::RequireUnoccupied)
                               .Succeeded());
            VERIFY_IS_TRUE(ReplayTaskWorktree(fixture, prepared).Succeeded());
        }

    }

    class GitWorktreeBaselineTests
    {
        TEST_CLASS(GitWorktreeBaselineTests);

        TEST_METHOD(UsesSourceHeadForCleanSnapshotAndRecoversIdempotently);
        TEST_METHOD(CommitsDirtyBaselineWithFallbackIdentityAndDisabledHooks);
        TEST_METHOD(RejectsIncompleteIdentityAndPreservesStagedCandidate);
        TEST_METHOD(PreservesStagedCandidateOnSigningFailureAndRetries);
        TEST_METHOD(RecoversCommitAfterMarkerReplacementFailure);
    };

    void GitWorktreeBaselineTests::UsesSourceHeadForCleanSnapshotAndRecoversIdempotently()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"clean baseline",
            TaskId{ "b1000000-0000-4000-8000-000000000001" });
        _prepareForBaseline(fixture, prepared);

        auto result = _baseline(fixture, prepared);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(result.Status == GitWorktreeBaselineStatus::Succeeded);
        VERIFY_ARE_EQUAL(prepared.Journal.SourceHead, result.BaselineCommit);
        VERIFY_IS_FALSE(result.CommitCreated);
        VERIFY_IS_TRUE(result.Marker.has_value());
        VERIFY_ARE_EQUAL(
            prepared.Journal.SourceHead,
            result.Marker->BaselineCommit);
        prepared.Marker = *result.Marker;

        result = _baseline(fixture, prepared);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            result.Status == GitWorktreeBaselineStatus::AlreadyCreated);
        VERIFY_IS_FALSE(result.CommitCreated);
        VERIFY_ARE_EQUAL(
            prepared.Journal.SourceHead,
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"rev-parse", L"--verify", L"HEAD" })));
    }

    void GitWorktreeBaselineTests::CommitsDirtyBaselineWithFallbackIdentityAndDisabledHooks()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"dirty baseline",
            TaskId{ "b2000000-0000-4000-8000-000000000002" });
        GitFixture::Write(prepared.Repository / L"tracked.txt", "tracked edit\n");
        GitFixture::Write(prepared.Repository / L"new.txt", "new file\n");
        RecaptureTaskWorktree(fixture, prepared);
        const auto before = ObserveGitSourceState(
            fixture,
            prepared.Repository,
            { L"tracked.txt", L"new.txt" });
        _prepareForBaseline(fixture, prepared);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"commit.gpgSign", L"false" })
                           .Succeeded());

        const auto hooks = fixture.Temporary.Path() / L"baseline hooks";
        VERIFY_IS_TRUE(std::filesystem::create_directory(hooks));
        const auto preCommit = hooks / L"pre-commit";
        GitFixture::Write(preCommit, "#!/bin/sh\nexit 97\n");
        std::filesystem::permissions(
            preCommit,
            std::filesystem::perms::owner_exec |
                std::filesystem::perms::group_exec |
                std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  {
                                      L"config",
                                      L"core.hooksPath",
                                      hooks.native(),
                                  })
                           .Succeeded());
        const auto hookProof = fixture.Run(
            prepared.Layout.WorktreePath,
            { L"hook", L"run", L"pre-commit" });
        VERIFY_IS_TRUE(hookProof.Status == GitCommandStatus::Completed);
        VERIFY_ARE_EQUAL(DWORD{ 97 }, hookProof.ExitCode);

        bool fallbackSeen{};
        GitCommandRunner runner;
        GitWorktreeBaselineCreator creator{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                if (request.Arguments ==
                        std::vector<std::wstring>{
                            L"config", L"--get", L"user.name" } ||
                    request.Arguments ==
                        std::vector<std::wstring>{
                            L"config", L"--get", L"user.email" })
                {
                    GitCommandResult result;
                    result.Status = GitCommandStatus::Completed;
                    result.ExitCode = 1;
                    return result;
                }
                if (std::ranges::find(request.Arguments, L"commit") !=
                    request.Arguments.end())
                {
                    fallbackSeen =
                        std::ranges::find(
                            request.Arguments,
                            L"user.name=Richer Task Runtime") !=
                            request.Arguments.end() &&
                        std::ranges::find(
                            request.Arguments,
                            L"user.email=task-runtime@localhost") !=
                            request.Arguments.end();
                }
                return runner.Run(request);
            }
        };
        const auto result = creator.Create(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(result.CommitCreated);
        VERIFY_IS_TRUE(result.UsedFallbackIdentity);
        VERIFY_IS_TRUE(fallbackSeen);
        VERIFY_ARE_NOT_EQUAL(prepared.Journal.SourceHead, result.BaselineCommit);
        VERIFY_ARE_EQUAL(
            std::string{ "Richer Task Runtime <task-runtime@localhost>" },
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"show", L"-s", L"--format=%an <%ae>", L"HEAD" })));
        VERIFY_IS_TRUE(RunCompleted(
                           fixture,
                           prepared.Layout.WorktreePath,
                           { L"status", L"--porcelain" })
                           .Stdout.empty());
        const auto after = ObserveGitSourceState(
            fixture,
            prepared.Repository,
            { L"tracked.txt", L"new.txt" });
        VERIFY_IS_TRUE(SameGitSourceState(before, after));
    }

    void GitWorktreeBaselineTests::RejectsIncompleteIdentityAndPreservesStagedCandidate()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"identity baseline",
            TaskId{ "b2500000-0000-4000-8000-000000000005" });
        GitFixture::Write(prepared.Repository / L"tracked.txt", "identity edit\n");
        RecaptureTaskWorktree(fixture, prepared);
        _prepareForBaseline(fixture, prepared);

        GitCommandRunner runner;
        GitWorktreeBaselineCreator creator{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                if (request.Arguments ==
                    std::vector<std::wstring>{
                        L"config", L"--get", L"user.name" })
                {
                    GitCommandResult result;
                    result.Status = GitCommandStatus::Completed;
                    result.ExitCode = 1;
                    return result;
                }
                if (request.Arguments ==
                    std::vector<std::wstring>{
                        L"config", L"--get", L"user.email" })
                {
                    GitCommandResult result;
                    result.Status = GitCommandStatus::Completed;
                    result.ExitCode = 0;
                    result.Stdout = "only-email@example.invalid\n";
                    return result;
                }
                return runner.Run(request);
            }
        };
        const auto result = creator.Create(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(
            result.Status == GitWorktreeBaselineStatus::IdentityFailure);
        VERIFY_IS_TRUE(result.Preserved);
        VERIFY_ARE_EQUAL(
            prepared.Journal.SourceHead,
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"rev-parse", L"--verify", L"HEAD" })));
        VERIFY_IS_FALSE(RunCompleted(
                            fixture,
                            prepared.Layout.WorktreePath,
                            { L"status", L"--porcelain" })
                            .Stdout.empty());
    }

    void GitWorktreeBaselineTests::PreservesStagedCandidateOnSigningFailureAndRetries()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"signing baseline",
            TaskId{ "b3000000-0000-4000-8000-000000000003" });
        GitFixture::Write(prepared.Repository / L"tracked.txt", "signed edit\n");
        RecaptureTaskWorktree(fixture, prepared);
        _prepareForBaseline(fixture, prepared);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"user.name", L"Signing Test" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"user.email", L"signing@example.invalid" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"commit.gpgSign", L"true" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"gpg.format", L"openpgp" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"gpg.program", L"missing-task-gpg.exe" })
                           .Succeeded());

        const auto failed = _baseline(fixture, prepared);
        VERIFY_IS_TRUE(
            failed.Status == GitWorktreeBaselineStatus::SigningFailure);
        VERIFY_IS_TRUE(failed.SigningConfigured);
        VERIFY_IS_TRUE(failed.Preserved);
        VERIFY_ARE_EQUAL(
            prepared.Journal.SourceHead,
            SingleLine(RunCompleted(
                fixture,
                prepared.Layout.WorktreePath,
                { L"rev-parse", L"--verify", L"HEAD" })));

        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"commit.gpgSign", L"false" })
                           .Succeeded());
        const auto recovered = _baseline(fixture, prepared);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(recovered.CommitCreated);
        VERIFY_ARE_NOT_EQUAL(
            prepared.Journal.SourceHead,
            recovered.BaselineCommit);
    }

    void GitWorktreeBaselineTests::RecoversCommitAfterMarkerReplacementFailure()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"marker recovery baseline",
            TaskId{ "b4000000-0000-4000-8000-000000000004" });
        GitFixture::Write(prepared.Repository / L"new.txt", "baseline data\n");
        RecaptureTaskWorktree(fixture, prepared);
        _prepareForBaseline(fixture, prepared);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"commit.gpgSign", L"false" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"user.name", L"Marker Test" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  { L"config", L"user.email", L"marker@example.invalid" })
                           .Succeeded());

        UniqueHandle markerHandle{ CreateFileW(
            prepared.Layout.MarkerPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr) };
        VERIFY_IS_TRUE(static_cast<bool>(markerHandle));
        const auto failed = _baseline(fixture, prepared);
        VERIFY_IS_TRUE(
            failed.Status == GitWorktreeBaselineStatus::MarkerUpdateFailed);
        VERIFY_IS_TRUE(failed.CommitCreated);
        VERIFY_IS_TRUE(failed.Preserved);
        VERIFY_IS_FALSE(failed.BaselineCommit.empty());
        markerHandle.Reset();

        const auto recovered = _baseline(fixture, prepared);
        VERIFY_IS_TRUE(recovered.Succeeded());
        VERIFY_IS_TRUE(
            recovered.Status == GitWorktreeBaselineStatus::AlreadyCreated);
        VERIFY_IS_FALSE(recovered.CommitCreated);
        VERIFY_ARE_EQUAL(failed.BaselineCommit, recovered.BaselineCommit);
        VERIFY_IS_TRUE(recovered.Marker.has_value());
        VERIFY_ARE_EQUAL(
            failed.BaselineCommit,
            recovered.Marker->BaselineCommit);
    }
}
