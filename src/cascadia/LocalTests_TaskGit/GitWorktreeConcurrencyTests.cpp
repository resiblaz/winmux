// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitWorktreeCheckpoint.h"
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
        bool _checkpointCriticalCommand(const GitCommandRequest& request)
        {
            return !request.Arguments.empty() &&
                   request.Arguments.front() == L"merge-base";
        }
    }

    class GitWorktreeConcurrencyTests
    {
        TEST_CLASS(GitWorktreeConcurrencyTests);

        TEST_METHOD(SameRepositoryCheckpointCriticalSectionsSerialize);
        TEST_METHOD(DifferentRepositoryCheckpointCriticalSectionsRunInParallel);
    };

    void GitWorktreeConcurrencyTests::SameRepositoryCheckpointCriticalSectionsSerialize()
    {
        GitFixture fixture;
        auto prepared = PrepareTaskWorktree(
            fixture,
            L"same repository concurrency",
            TaskId{ "f1000000-0000-4000-8000-000000000001" });
        PrepareReadyTaskWorktree(fixture, prepared);
        auto firstEntered = MakeEvent();
        auto secondEntered = MakeEvent();
        auto releaseFirst = MakeEvent();
        VERIFY_IS_TRUE(static_cast<bool>(firstEntered));
        VERIFY_IS_TRUE(static_cast<bool>(secondEntered));
        VERIFY_IS_TRUE(static_cast<bool>(releaseFirst));

        GitCommandRunner runner;
        GitWorktreeCheckpointManager firstManager{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                if (_checkpointCriticalCommand(request))
                {
                    SetEvent(firstEntered.Get());
                    if (WaitForSingleObject(releaseFirst.Get(), 5000) !=
                        WAIT_OBJECT_0)
                    {
                        GitCommandResult failed;
                        failed.Status = GitCommandStatus::TimedOut;
                        failed.SystemError = WAIT_TIMEOUT;
                        return failed;
                    }
                }
                return runner.Run(request);
            }
        };
        GitWorktreeCheckpointManager secondManager{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                if (_checkpointCriticalCommand(request))
                {
                    SetEvent(secondEntered.Get());
                }
                return runner.Run(request);
            }
        };

        GitWorktreeCheckpointStartResult first;
        GitWorktreeCheckpointStartResult second;
        std::thread firstThread{ [&] {
            first = firstManager.CaptureStart(
                prepared.Layout,
                prepared.Marker,
                prepared.Journal,
                fixture.Cancellation.Get());
        } };
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(firstEntered.Get(), 5000));
        std::thread secondThread{ [&] {
            second = secondManager.CaptureStart(
                prepared.Layout,
                prepared.Marker,
                prepared.Journal,
                fixture.Cancellation.Get());
        } };
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_TIMEOUT },
            WaitForSingleObject(secondEntered.Get(), 100));
        VERIFY_IS_TRUE(SetEvent(releaseFirst.Get()) != FALSE);
        firstThread.join();
        secondThread.join();

        VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_IS_TRUE(second.Succeeded());
        VERIFY_ARE_EQUAL(first.StartCommit, second.StartCommit);
        VERIFY_ARE_EQUAL(first.StateDigest, second.StateDigest);
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(secondEntered.Get(), 0));
    }

    void GitWorktreeConcurrencyTests::DifferentRepositoryCheckpointCriticalSectionsRunInParallel()
    {
        GitFixture fixture;
        auto firstPrepared = PrepareTaskWorktree(
            fixture,
            L"parallel repository one",
            TaskId{ "f2000000-0000-4000-8000-000000000002" });
        auto secondPrepared = PrepareTaskWorktree(
            fixture,
            L"parallel repository two",
            TaskId{ "f3000000-0000-4000-8000-000000000003" });
        PrepareReadyTaskWorktree(fixture, firstPrepared);
        PrepareReadyTaskWorktree(fixture, secondPrepared);
        auto bothEntered = MakeEvent();
        VERIFY_IS_TRUE(static_cast<bool>(bothEntered));
        std::atomic_uint32_t arrived{};
        std::atomic_bool barrierTimedOut{};
        GitCommandRunner runner;
        const auto executor = [&](const GitCommandRequest& request) {
            if (_checkpointCriticalCommand(request))
            {
                if (arrived.fetch_add(1, std::memory_order_acq_rel) + 1 == 2)
                {
                    SetEvent(bothEntered.Get());
                }
                if (WaitForSingleObject(bothEntered.Get(), 2000) != WAIT_OBJECT_0)
                {
                    barrierTimedOut.store(true, std::memory_order_release);
                }
            }
            return runner.Run(request);
        };
        GitWorktreeCheckpointManager firstManager{
            fixture.GitExecutable,
            executor
        };
        GitWorktreeCheckpointManager secondManager{
            fixture.GitExecutable,
            executor
        };
        GitWorktreeCheckpointStartResult first;
        GitWorktreeCheckpointStartResult second;
        std::thread firstThread{ [&] {
            first = firstManager.CaptureStart(
                firstPrepared.Layout,
                firstPrepared.Marker,
                firstPrepared.Journal,
                fixture.Cancellation.Get());
        } };
        std::thread secondThread{ [&] {
            second = secondManager.CaptureStart(
                secondPrepared.Layout,
                secondPrepared.Marker,
                secondPrepared.Journal,
                fixture.Cancellation.Get());
        } };
        firstThread.join();
        secondThread.join();

        VERIFY_IS_FALSE(barrierTimedOut.load(std::memory_order_acquire));
        VERIFY_ARE_EQUAL(uint32_t{ 2 }, arrived.load(std::memory_order_acquire));
        VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_IS_TRUE(second.Succeeded());
    }
}
