// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitRepositoryInspector.h"
#include "GitRepositoryLock.h"
#include "GitRepositoryTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskExecution;
using namespace TaskExecutionLocalTests::TestSupport;

namespace TaskExecutionLocalTests
{
    class GitRepositoryInspectorTests
    {
        TEST_CLASS(GitRepositoryInspectorTests);

        TEST_METHOD(InspectsMainLinkedAndDetachedSha1Worktrees);
        TEST_METHOD(InspectsSha256AndRejectsBareUnbornAndPlainDirectories);
        TEST_METHOD(RejectsOldGitAndPropagatesCancellationTimeoutAndInvalidOutput);
        TEST_METHOD(CommonDirectoryLockDeadlineAndCancellationRemainBounded);
    };

    void GitRepositoryInspectorTests::InspectsMainLinkedAndDetachedSha1Worktrees()
    {
        GitRepositoryFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"主 repository";
        const auto linked = fixture.Temporary.Path() / L"linked 工作树";
        const auto detached = fixture.Temporary.Path() / L"detached 工作树";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        const auto nestedPath = repository / L"src" / L"nested";
        VERIFY_IS_TRUE(std::filesystem::create_directories(nestedPath));
        VERIFY_IS_TRUE(fixture.Run(repository, {
                                        L"worktree",
                                        L"add",
                                        L"-b",
                                        L"linked-分支",
                                        linked.native(),
                                        L"HEAD",
                                    }).Succeeded());
        VERIFY_IS_TRUE(fixture.Run(repository, {
                                        L"worktree",
                                        L"add",
                                        L"--detach",
                                        detached.native(),
                                        L"HEAD",
                                    }).Succeeded());

        GitRepositoryInspector inspector{ fixture.GitExecutable };
        const auto main = inspector.Inspect(
            repository,
            fixture.Cancellation.Get());
        const auto nested = inspector.Inspect(
            nestedPath,
            fixture.Cancellation.Get());
        const auto linkedResult = inspector.Inspect(
            linked,
            fixture.Cancellation.Get());
        const auto detachedResult = inspector.Inspect(
            detached,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(main.Succeeded());
        VERIFY_IS_TRUE(nested.Succeeded());
        VERIFY_IS_TRUE(linkedResult.Succeeded());
        VERIFY_IS_TRUE(detachedResult.Succeeded());

        VERIFY_IS_TRUE(main.Inspection->ObjectFormat == GitObjectFormat::Sha1);
        VERIFY_ARE_EQUAL(size_t{ 40 }, main.Inspection->Head.size());
        VERIFY_ARE_EQUAL(std::string{ "refs/heads/main" }, main.Inspection->BranchRef);
        VERIFY_IS_FALSE(main.Inspection->LinkedWorktree);
        VERIFY_IS_FALSE(main.Inspection->DetachedHead);
        VERIFY_IS_GREATER_THAN(main.Inspection->CapturedAtUnixMilliseconds, int64_t{ 0 });
        VERIFY_ARE_EQUAL(
            main.Inspection->WorktreeIdentity.Digest(),
            nested.Inspection->WorktreeIdentity.Digest());

        VERIFY_IS_TRUE(linkedResult.Inspection->LinkedWorktree);
        VERIFY_IS_FALSE(linkedResult.Inspection->DetachedHead);
        VERIFY_ARE_EQUAL(
            std::string{ "refs/heads/" },
            linkedResult.Inspection->BranchRef.substr(0, 11));
        VERIFY_IS_GREATER_THAN(
            linkedResult.Inspection->BranchRef.size(),
            size_t{ 11 });
        VERIFY_ARE_EQUAL(
            main.Inspection->CommonDirectoryIdentity.Digest(),
            linkedResult.Inspection->CommonDirectoryIdentity.Digest());
        VERIFY_ARE_NOT_EQUAL(
            linkedResult.Inspection->CommonDirectoryIdentity.Digest(),
            linkedResult.Inspection->GitDirectoryIdentity.Digest());

        VERIFY_IS_TRUE(detachedResult.Inspection->LinkedWorktree);
        VERIFY_IS_TRUE(detachedResult.Inspection->DetachedHead);
        VERIFY_IS_TRUE(detachedResult.Inspection->BranchRef.empty());
        VERIFY_ARE_EQUAL(main.Inspection->Head, detachedResult.Inspection->Head);
    }

    void GitRepositoryInspectorTests::InspectsSha256AndRejectsBareUnbornAndPlainDirectories()
    {
        GitRepositoryFixture fixture;
        const auto sha256 = fixture.Temporary.Path() / L"sha256";
        const auto bare = fixture.Temporary.Path() / L"bare";
        const auto unborn = fixture.Temporary.Path() / L"unborn";
        VERIFY_IS_TRUE(fixture.Initialize(sha256, true));
        VERIFY_IS_TRUE(fixture.InitializeBare(bare));
        VERIFY_IS_TRUE(fixture.Initialize(unborn, false, false));

        GitRepositoryInspector inspector{ fixture.GitExecutable };
        const auto sha256Result = inspector.Inspect(
            sha256,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(sha256Result.Succeeded());
        VERIFY_IS_TRUE(
            sha256Result.Inspection->ObjectFormat == GitObjectFormat::Sha256);
        VERIFY_ARE_EQUAL(size_t{ 64 }, sha256Result.Inspection->Head.size());
        VERIFY_IS_TRUE(
            sha256Result.Inspection->Version.Major > 2 ||
            (sha256Result.Inspection->Version.Major == 2 &&
             sha256Result.Inspection->Version.Minor >= 38));

        VERIFY_IS_TRUE(
            inspector.Inspect(bare, fixture.Cancellation.Get()).Status ==
            GitRepositoryInspectionStatus::BareRepository);
        VERIFY_IS_TRUE(
            inspector.Inspect(unborn, fixture.Cancellation.Get()).Status ==
            GitRepositoryInspectionStatus::UnbornRepository);
        VERIFY_IS_TRUE(
            inspector.Inspect(
                         fixture.Temporary.Path(),
                         fixture.Cancellation.Get())
                    .Status == GitRepositoryInspectionStatus::NotRepository);
    }

    void GitRepositoryInspectorTests::RejectsOldGitAndPropagatesCancellationTimeoutAndInvalidOutput()
    {
        GitRepositoryFixture fixture;
        size_t calls{};
        GitRepositoryInspector old{
            fixture.GitExecutable,
            [&](const GitCommandRequest&) {
                ++calls;
                return GitCommandResult{
                    .Status = GitCommandStatus::Completed,
                    .ExitCode = 0,
                    .Stdout = "git version 2.37.9\n",
                };
            }
        };
        VERIFY_IS_TRUE(
            old.Inspect(
                   fixture.Temporary.Path(),
                   fixture.Cancellation.Get())
                    .Status == GitRepositoryInspectionStatus::UnsupportedGitVersion);
        VERIFY_ARE_EQUAL(size_t{ 1 }, calls);

        GitRepositoryInspector timedOut{
            fixture.GitExecutable,
            [](const GitCommandRequest&) {
                return GitCommandResult{ .Status = GitCommandStatus::TimedOut };
            }
        };
        VERIFY_IS_TRUE(
            timedOut.Inspect(
                        fixture.Temporary.Path(),
                        fixture.Cancellation.Get())
                    .Status == GitRepositoryInspectionStatus::TimedOut);

        GitRepositoryInspector invalidOutput{
            fixture.GitExecutable,
            [](const GitCommandRequest&) {
                return GitCommandResult{
                    .Status = GitCommandStatus::Completed,
                    .ExitCode = 0,
                    .Stdout = std::string{ "\xff", 1 },
                };
            }
        };
        VERIFY_IS_TRUE(
            invalidOutput.Inspect(
                             fixture.Temporary.Path(),
                             fixture.Cancellation.Get())
                    .Status == GitRepositoryInspectionStatus::InvalidOutput);

        auto cancelled = MakeManualResetEvent(true);
        VERIFY_IS_TRUE(
            old.Inspect(fixture.Temporary.Path(), cancelled.Get()).Status ==
            GitRepositoryInspectionStatus::Cancelled);
        VERIFY_IS_TRUE(
            old.Inspect(
                   fixture.Temporary.Path(),
                   fixture.Cancellation.Get(),
                   MaximumGitCommandTimeoutMilliseconds + 1)
                    .Status == GitRepositoryInspectionStatus::InvalidRequest);
    }

    void GitRepositoryInspectorTests::CommonDirectoryLockDeadlineAndCancellationRemainBounded()
    {
        GitRepositoryFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"locked";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        const auto common = ResolveCanonicalDirectoryIdentity(
            repository / L".git",
            DirectoryIdentityPolicy::Worktree);
        VERIFY_IS_TRUE(common.Succeeded());
        auto ownerCancellation = MakeManualResetEvent();
        auto owner = AcquireGitRepositoryLock(
            *common.Identity,
            ownerCancellation.Get(),
            1000);
        VERIFY_IS_TRUE(owner.Acquired());

        GitRepositoryInspector inspector{ fixture.GitExecutable };
        std::atomic<int> timeoutStatus{ -1 };
        std::thread timed{ [&] {
            const auto result = inspector.Inspect(
                repository,
                fixture.Cancellation.Get(),
                100);
            timeoutStatus.store(
                static_cast<int>(result.Status),
                std::memory_order_release);
        } };
        timed.join();
        VERIFY_IS_TRUE(
            timeoutStatus.load(std::memory_order_acquire) ==
            static_cast<int>(GitRepositoryInspectionStatus::TimedOut));

        auto cancellation = MakeManualResetEvent();
        std::atomic<int> cancelledStatus{ -1 };
        std::thread cancelled{ [&] {
            const auto result = inspector.Inspect(
                repository,
                cancellation.Get(),
                5000);
            cancelledStatus.store(
                static_cast<int>(result.Status),
                std::memory_order_release);
        } };
        std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });
        SetEvent(cancellation.Get());
        cancelled.join();
        VERIFY_IS_TRUE(
            cancelledStatus.load(std::memory_order_acquire) ==
            static_cast<int>(GitRepositoryInspectionStatus::Cancelled));
        VERIFY_IS_TRUE(
            owner.Lock.Release() == TaskDirectoryLockReleaseStatus::Released);
    }
}
