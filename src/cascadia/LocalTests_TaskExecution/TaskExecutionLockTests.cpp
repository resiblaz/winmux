// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "DirectDirectoryLease.h"
#include "DirectoryMutexInternal.h"
#include "GitRepositoryLock.h"
#include "TaskExecutionSecurity.h"
#include "TaskExecutionTestSupport.h"

#include <sddl.h>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskExecution;
using namespace TaskExecutionLocalTests::TestSupport;

namespace TaskExecutionLocalTests
{
    namespace
    {
        CanonicalDirectoryIdentity _resolve(
            const std::filesystem::path& path,
            const DirectoryIdentityPolicy policy)
        {
            const auto result = ResolveCanonicalDirectoryIdentity(path, policy);
            VERIFY_IS_TRUE(result.Succeeded());
            return *result.Identity;
        }

        HANDLE _createMutexWithSddl(
            const std::wstring& name,
            const std::wstring& sddl,
            const bool initialOwner)
        {
            PSECURITY_DESCRIPTOR descriptor{};
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl.c_str(),
                    SDDL_REVISION_1,
                    &descriptor,
                    nullptr))
            {
                return nullptr;
            }
            SECURITY_ATTRIBUTES attributes{
                sizeof(SECURITY_ATTRIBUTES),
                descriptor,
                FALSE,
            };
            const auto mutex = CreateMutexW(
                &attributes,
                initialOwner,
                name.c_str());
            LocalFree(descriptor);
            return mutex;
        }

        HANDLE _abandonMutex(
            const CanonicalDirectoryIdentity& identity,
            const details::DirectoryMutexScope scope,
            std::wstring& userSid)
        {
            const auto mutexIdentity = details::BuildDirectoryMutexIdentity(
                identity,
                scope);
            VERIFY_IS_TRUE(mutexIdentity.Succeeded());
            userSid = mutexIdentity.UserSid;
            auto security = details::CreateTaskExecutionSecurityAttributes(userSid);
            VERIFY_IS_TRUE(security.Succeeded);
            auto ownerReady = MakeManualResetEvent();
            std::atomic<HANDLE> abandonedHandle{};
            std::thread abandonedOwner{ [&] {
                abandonedHandle.store(
                    CreateMutexW(
                        const_cast<SECURITY_ATTRIBUTES*>(security.Security.Attributes()),
                        TRUE,
                        mutexIdentity.MutexName.c_str()),
                    std::memory_order_release);
                SetEvent(ownerReady.Get());
            } };
            VERIFY_ARE_EQUAL(
                DWORD{ WAIT_OBJECT_0 },
                WaitForSingleObject(ownerReady.Get(), 5000));
            abandonedOwner.join();
            const auto handle = abandonedHandle.load(std::memory_order_acquire);
            VERIFY_IS_NOT_NULL(handle);
            return handle;
        }
    }

    class TaskExecutionLockTests
    {
        TEST_CLASS(TaskExecutionLockTests);

        TEST_METHOD(DirectLeaseIsStrictFifoAndRejectsReentrantOwner);
        TEST_METHOD(DirectLeaseCancellationRemovesMiddleWaiter);
        TEST_METHOD(DirectLeaseReportsAbandonedNamedMutex);
        TEST_METHOD(GitLockHonorsTimeoutCancellationAndDirectoryParallelism);
        TEST_METHOD(GitLockIsThreadAffineAndReportsAbandonedOwnership);
        TEST_METHOD(MutexNamesAreScopedStableAndRejectBroadPrecreation);
        TEST_METHOD(RepeatedAcquireReleaseDoesNotLeakHandlesOrQueueParticipants);
    };

    void TaskExecutionLockTests::DirectLeaseIsStrictFifoAndRejectsReentrantOwner()
    {
        TemporaryDirectory temporary;
        const auto identity = _resolve(
            temporary.Path(),
            DirectoryIdentityPolicy::Direct);
        DirectDirectoryLeaseManager manager;
        auto firstCancellation = MakeManualResetEvent();
        auto secondCancellation = MakeManualResetEvent();
        auto thirdCancellation = MakeManualResetEvent();
        auto secondAcquired = MakeManualResetEvent();
        auto thirdAcquired = MakeManualResetEvent();
        auto releaseSecond = MakeManualResetEvent();
        auto releaseThird = MakeManualResetEvent();
        VERIFY_IS_TRUE(static_cast<bool>(firstCancellation));
        VERIFY_IS_TRUE(static_cast<bool>(secondCancellation));
        VERIFY_IS_TRUE(static_cast<bool>(thirdCancellation));

        auto first = manager.Acquire(identity, firstCancellation.Get());
        VERIFY_IS_TRUE(first.Acquired());
        VERIFY_ARE_EQUAL(size_t{ 1 }, manager.ParticipantCount(identity));

        DirectDirectoryLeaseManager alternateManager;
        const auto reentrant = alternateManager.Acquire(
            identity,
            firstCancellation.Get());
        VERIFY_IS_TRUE(reentrant.Status == TaskDirectoryLockStatus::ReentrantAcquire);

        std::mutex orderMutex;
        std::vector<int> order;
        std::atomic<int> secondStatus{ -1 };
        std::atomic<int> thirdStatus{ -1 };
        std::atomic<int> secondReleaseStatus{ -1 };
        std::atomic<int> thirdReleaseStatus{ -1 };

        std::thread second{ [&] {
            auto result = manager.Acquire(identity, secondCancellation.Get());
            secondStatus.store(static_cast<int>(result.Status), std::memory_order_release);
            if (result.Acquired())
            {
                {
                    std::scoped_lock lock{ orderMutex };
                    order.push_back(2);
                }
                SetEvent(secondAcquired.Get());
                WaitForSingleObject(releaseSecond.Get(), 5000);
                secondReleaseStatus.store(
                    static_cast<int>(result.Lease.Release()),
                    std::memory_order_release);
            }
        } };
        VERIFY_IS_TRUE(WaitUntil([&] {
            return manager.ParticipantCount(identity) == 2;
        }));

        std::thread third{ [&] {
            auto result = manager.Acquire(identity, thirdCancellation.Get());
            thirdStatus.store(static_cast<int>(result.Status), std::memory_order_release);
            if (result.Acquired())
            {
                {
                    std::scoped_lock lock{ orderMutex };
                    order.push_back(3);
                }
                SetEvent(thirdAcquired.Get());
                WaitForSingleObject(releaseThird.Get(), 5000);
                thirdReleaseStatus.store(
                    static_cast<int>(result.Lease.Release()),
                    std::memory_order_release);
            }
        } };
        VERIFY_IS_TRUE(WaitUntil([&] {
            return manager.ParticipantCount(identity) == 3;
        }));

        VERIFY_IS_TRUE(
            first.Lease.Release() == TaskDirectoryLockReleaseStatus::Released);
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(secondAcquired.Get(), 5000));
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_TIMEOUT },
            WaitForSingleObject(thirdAcquired.Get(), 50));
        SetEvent(releaseSecond.Get());
        second.join();
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(thirdAcquired.Get(), 5000));
        SetEvent(releaseThird.Get());
        third.join();

        VERIFY_IS_TRUE(
            secondStatus.load(std::memory_order_acquire) ==
            static_cast<int>(TaskDirectoryLockStatus::Acquired));
        VERIFY_IS_TRUE(
            thirdStatus.load(std::memory_order_acquire) ==
            static_cast<int>(TaskDirectoryLockStatus::Acquired));
        VERIFY_IS_TRUE(
            secondReleaseStatus.load(std::memory_order_acquire) ==
            static_cast<int>(TaskDirectoryLockReleaseStatus::Released));
        VERIFY_IS_TRUE(
            thirdReleaseStatus.load(std::memory_order_acquire) ==
            static_cast<int>(TaskDirectoryLockReleaseStatus::Released));
        VERIFY_IS_TRUE((order == std::vector<int>{ 2, 3 }));
        VERIFY_ARE_EQUAL(size_t{ 0 }, manager.ParticipantCount(identity));
    }

    void TaskExecutionLockTests::DirectLeaseCancellationRemovesMiddleWaiter()
    {
        TemporaryDirectory temporary;
        const auto identity = _resolve(
            temporary.Path(),
            DirectoryIdentityPolicy::Direct);
        DirectDirectoryLeaseManager manager;
        auto firstCancellation = MakeManualResetEvent();
        auto secondCancellation = MakeManualResetEvent();
        auto thirdCancellation = MakeManualResetEvent();
        auto secondDone = MakeManualResetEvent();
        auto thirdAcquired = MakeManualResetEvent();
        auto releaseThird = MakeManualResetEvent();

        auto first = manager.Acquire(identity, firstCancellation.Get());
        VERIFY_IS_TRUE(first.Acquired());
        std::atomic<int> secondStatus{ -1 };
        std::atomic<int> thirdStatus{ -1 };

        std::thread second{ [&] {
            auto result = manager.Acquire(identity, secondCancellation.Get());
            secondStatus.store(static_cast<int>(result.Status), std::memory_order_release);
            SetEvent(secondDone.Get());
        } };
        VERIFY_IS_TRUE(WaitUntil([&] {
            return manager.ParticipantCount(identity) == 2;
        }));

        std::thread third{ [&] {
            auto result = manager.Acquire(identity, thirdCancellation.Get());
            thirdStatus.store(static_cast<int>(result.Status), std::memory_order_release);
            if (result.Acquired())
            {
                SetEvent(thirdAcquired.Get());
                WaitForSingleObject(releaseThird.Get(), 5000);
                (void)result.Lease.Release();
            }
        } };
        VERIFY_IS_TRUE(WaitUntil([&] {
            return manager.ParticipantCount(identity) == 3;
        }));

        SetEvent(secondCancellation.Get());
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(secondDone.Get(), 5000));
        second.join();
        VERIFY_IS_TRUE(
            secondStatus.load(std::memory_order_acquire) ==
            static_cast<int>(TaskDirectoryLockStatus::Cancelled));
        VERIFY_ARE_EQUAL(size_t{ 2 }, manager.ParticipantCount(identity));

        VERIFY_IS_TRUE(
            first.Lease.Release() == TaskDirectoryLockReleaseStatus::Released);
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(thirdAcquired.Get(), 5000));
        SetEvent(releaseThird.Get());
        third.join();
        VERIFY_IS_TRUE(
            thirdStatus.load(std::memory_order_acquire) ==
            static_cast<int>(TaskDirectoryLockStatus::Acquired));
        VERIFY_ARE_EQUAL(size_t{ 0 }, manager.ParticipantCount(identity));

        const auto invalidCancellation = manager.Acquire(identity, nullptr);
        VERIFY_IS_TRUE(
            invalidCancellation.Status ==
            TaskDirectoryLockStatus::InvalidCancellation);
        const auto worktreeIdentity = _resolve(
            temporary.Path(),
            DirectoryIdentityPolicy::Worktree);
        const auto wrongPolicy = manager.Acquire(
            worktreeIdentity,
            firstCancellation.Get());
        VERIFY_IS_TRUE(
            wrongPolicy.Status == TaskDirectoryLockStatus::InvalidIdentity);
    }

    void TaskExecutionLockTests::DirectLeaseReportsAbandonedNamedMutex()
    {
        TemporaryDirectory temporary;
        const auto identity = _resolve(
            temporary.Path(),
            DirectoryIdentityPolicy::Direct);
        std::wstring userSid;
        const auto abandonedHandle = _abandonMutex(
            identity,
            details::DirectoryMutexScope::Direct,
            userSid);

        DirectDirectoryLeaseManager manager;
        auto cancellation = MakeManualResetEvent();
        auto recovered = manager.Acquire(identity, cancellation.Get());
        VERIFY_IS_TRUE(recovered.Acquired());
        VERIFY_IS_TRUE(recovered.Lease.Abandoned());
        const auto validated = details::ValidateTaskExecutionMutexSecurity(
            recovered.Lease.Handle(),
            userSid);
        VERIFY_IS_TRUE(validated.Succeeded);
        VERIFY_IS_TRUE(
            recovered.Lease.Release() == TaskDirectoryLockReleaseStatus::Released);
        CloseHandle(abandonedHandle);
    }

    void TaskExecutionLockTests::GitLockHonorsTimeoutCancellationAndDirectoryParallelism()
    {
        TemporaryDirectory temporary;
        const auto firstPath = temporary.Path() / L"first";
        const auto secondPath = temporary.Path() / L"second";
        VERIFY_IS_TRUE(std::filesystem::create_directory(firstPath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(secondPath));
        const auto firstIdentity = _resolve(
            firstPath,
            DirectoryIdentityPolicy::Worktree);
        const auto secondIdentity = _resolve(
            secondPath,
            DirectoryIdentityPolicy::Worktree);
        auto ownerCancellation = MakeManualResetEvent();
        auto owner = AcquireGitRepositoryLock(
            firstIdentity,
            ownerCancellation.Get(),
            5000);
        VERIFY_IS_TRUE(owner.Acquired());

        std::atomic<int> timeoutStatus{ -1 };
        auto timeoutCancellation = MakeManualResetEvent();
        std::thread timed{ [&] {
            const auto result = AcquireGitRepositoryLock(
                firstIdentity,
                timeoutCancellation.Get(),
                50);
            timeoutStatus.store(static_cast<int>(result.Status), std::memory_order_release);
        } };
        timed.join();
        VERIFY_IS_TRUE(
            timeoutStatus.load(std::memory_order_acquire) ==
            static_cast<int>(TaskDirectoryLockStatus::TimedOut));

        std::atomic<int> cancelledStatus{ -1 };
        auto waitEntered = MakeManualResetEvent();
        auto cancellation = MakeManualResetEvent();
        std::thread cancelled{ [&] {
            SetEvent(waitEntered.Get());
            const auto result = AcquireGitRepositoryLock(
                firstIdentity,
                cancellation.Get(),
                5000);
            cancelledStatus.store(static_cast<int>(result.Status), std::memory_order_release);
        } };
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(waitEntered.Get(), 5000));
        std::this_thread::sleep_for(std::chrono::milliseconds{ 25 });
        SetEvent(cancellation.Get());
        cancelled.join();
        VERIFY_IS_TRUE(
            cancelledStatus.load(std::memory_order_acquire) ==
            static_cast<int>(TaskDirectoryLockStatus::Cancelled));

        auto otherCancellation = MakeManualResetEvent();
        auto other = AcquireGitRepositoryLock(
            secondIdentity,
            otherCancellation.Get(),
            1000);
        VERIFY_IS_TRUE(other.Acquired());
        VERIFY_IS_TRUE(
            other.Lock.Release() == TaskDirectoryLockReleaseStatus::Released);
        VERIFY_IS_TRUE(
            owner.Lock.Release() == TaskDirectoryLockReleaseStatus::Released);

        VERIFY_IS_TRUE(
            AcquireGitRepositoryLock(firstIdentity, ownerCancellation.Get(), 0).Status ==
            TaskDirectoryLockStatus::InvalidTimeout);
        VERIFY_IS_TRUE(
            AcquireGitRepositoryLock(
                firstIdentity,
                ownerCancellation.Get(),
                GitRepositoryLockTimeoutMilliseconds + 1)
                    .Status == TaskDirectoryLockStatus::InvalidTimeout);
        VERIFY_IS_TRUE(
            AcquireGitRepositoryLock(firstIdentity, nullptr, 1000).Status ==
            TaskDirectoryLockStatus::InvalidCancellation);
        const auto directIdentity = _resolve(
            firstPath,
            DirectoryIdentityPolicy::Direct);
        VERIFY_IS_TRUE(
            AcquireGitRepositoryLock(directIdentity, ownerCancellation.Get(), 1000).Status ==
            TaskDirectoryLockStatus::InvalidIdentity);
    }

    void TaskExecutionLockTests::GitLockIsThreadAffineAndReportsAbandonedOwnership()
    {
        TemporaryDirectory temporary;
        const auto identity = _resolve(
            temporary.Path(),
            DirectoryIdentityPolicy::Worktree);
        auto cancellation = MakeManualResetEvent();
        auto acquired = AcquireGitRepositoryLock(identity, cancellation.Get(), 1000);
        VERIFY_IS_TRUE(acquired.Acquired());

        std::atomic<int> wrongThreadRelease{ -1 };
        std::thread wrongThread{ [&] {
            wrongThreadRelease.store(
                static_cast<int>(acquired.Lock.Release()),
                std::memory_order_release);
        } };
        wrongThread.join();
        VERIFY_IS_TRUE(
            wrongThreadRelease.load(std::memory_order_acquire) ==
            static_cast<int>(TaskDirectoryLockReleaseStatus::WrongThread));
        VERIFY_IS_TRUE(static_cast<bool>(acquired.Lock));
        VERIFY_IS_TRUE(
            acquired.Lock.Release() == TaskDirectoryLockReleaseStatus::Released);

        std::wstring abandonedUserSid;
        const auto abandonedHandle = _abandonMutex(
            identity,
            details::DirectoryMutexScope::Git,
            abandonedUserSid);

        auto recovered = AcquireGitRepositoryLock(identity, cancellation.Get(), 1000);
        VERIFY_IS_TRUE(recovered.Acquired());
        VERIFY_IS_TRUE(recovered.Lock.Abandoned());
        const auto validated = details::ValidateTaskExecutionMutexSecurity(
            recovered.Lock.Handle(),
            abandonedUserSid);
        VERIFY_IS_TRUE(validated.Succeeded);
        VERIFY_IS_TRUE(
            recovered.Lock.Release() == TaskDirectoryLockReleaseStatus::Released);
        CloseHandle(abandonedHandle);
    }

    void TaskExecutionLockTests::MutexNamesAreScopedStableAndRejectBroadPrecreation()
    {
        TemporaryDirectory temporary;
        const auto directIdentity = _resolve(
            temporary.Path(),
            DirectoryIdentityPolicy::Direct);
        const auto worktreeIdentity = _resolve(
            temporary.Path(),
            DirectoryIdentityPolicy::Worktree);
        const auto direct = details::BuildDirectoryMutexIdentity(
            directIdentity,
            details::DirectoryMutexScope::Direct);
        const auto repeated = details::BuildDirectoryMutexIdentity(
            directIdentity,
            details::DirectoryMutexScope::Direct);
        const auto git = details::BuildDirectoryMutexIdentity(
            worktreeIdentity,
            details::DirectoryMutexScope::Git);
        VERIFY_IS_TRUE(direct.Succeeded());
        VERIFY_IS_TRUE(repeated.Succeeded());
        VERIFY_IS_TRUE(git.Succeeded());
        VERIFY_ARE_EQUAL(direct.MutexName, repeated.MutexName);
        VERIFY_ARE_EQUAL(direct.UserSid, repeated.UserSid);
        VERIFY_ARE_NOT_EQUAL(direct.MutexName, git.MutexName);
        VERIFY_IS_TRUE(direct.MutexName.starts_with(
            L"Global\\WT.TaskExecution.Direct.v1."));
        VERIFY_IS_TRUE(git.MutexName.starts_with(
            L"Global\\WT.TaskExecution.Git.v1."));

        const auto sddl = L"O:" + git.UserSid + L"G:" + git.UserSid +
                          L"D:P(A;;GA;;;WD)S:(ML;;NW;;;ME)";
        const auto broad = _createMutexWithSddl(
            git.MutexName,
            sddl,
            false);
        VERIFY_IS_NOT_NULL(broad);
        auto cancellation = MakeManualResetEvent();
        const auto rejected = AcquireGitRepositoryLock(
            worktreeIdentity,
            cancellation.Get(),
            1000);
        VERIFY_IS_TRUE(
            rejected.Status == TaskDirectoryLockStatus::SecurityFailure);
        VERIFY_IS_FALSE(rejected.Acquired());
        CloseHandle(broad);
    }

    void TaskExecutionLockTests::RepeatedAcquireReleaseDoesNotLeakHandlesOrQueueParticipants()
    {
        TemporaryDirectory temporary;
        const auto directIdentity = _resolve(
            temporary.Path(),
            DirectoryIdentityPolicy::Direct);
        const auto worktreeIdentity = _resolve(
            temporary.Path(),
            DirectoryIdentityPolicy::Worktree);
        DirectDirectoryLeaseManager manager;
        auto cancellation = MakeManualResetEvent();
        DWORD handlesBefore{};
        VERIFY_IS_TRUE(GetProcessHandleCount(
                           GetCurrentProcess(),
                           &handlesBefore) != FALSE);

        for (size_t iteration = 0; iteration < 100; ++iteration)
        {
            auto direct = manager.Acquire(directIdentity, cancellation.Get());
            VERIFY_IS_TRUE(direct.Acquired());
            VERIFY_IS_TRUE(
                direct.Lease.Release() ==
                TaskDirectoryLockReleaseStatus::Released);

            auto git = AcquireGitRepositoryLock(
                worktreeIdentity,
                cancellation.Get(),
                1000);
            VERIFY_IS_TRUE(git.Acquired());
            VERIFY_IS_TRUE(
                git.Lock.Release() ==
                TaskDirectoryLockReleaseStatus::Released);
        }

        DWORD handlesAfter{};
        VERIFY_IS_TRUE(GetProcessHandleCount(
                           GetCurrentProcess(),
                           &handlesAfter) != FALSE);
        VERIFY_IS_LESS_THAN_OR_EQUAL(handlesAfter, handlesBefore + 2);
        VERIFY_ARE_EQUAL(size_t{ 0 }, manager.ParticipantCount(directIdentity));
    }
}
