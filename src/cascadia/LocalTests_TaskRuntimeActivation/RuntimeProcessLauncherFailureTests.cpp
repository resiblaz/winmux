// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeProcessLauncherTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeProcessLauncherFailureTests
    {
        TEST_CLASS(RuntimeProcessLauncherFailureTests);

        TEST_METHOD(CreateFailureAndMalformedCreateResultCloseReturnedHandlesOnce);
        TEST_METHOD(ChildJobQueryFailureAndStillInJobRollbackBeforeResume);
        TEST_METHOD(ResumeFailureRollsBackSuspendedProcess);
        TEST_METHOD(RollbackFailureDominatesOriginalFailureAndStillClosesHandles);
    };

    void RuntimeProcessLauncherFailureTests::CreateFailureAndMalformedCreateResultCloseReturnedHandlesOnce()
    {
        UniqueActivationIdentity identity;
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            api->CreateProcessSucceeded = false;
            api->CreateProcessError = ERROR_ACCESS_DENIED;
            const auto launcher = MakeTestProcessLauncher(api);
            const auto result = launcher->Launch(ValidLaunchRequest(identity));
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::ProcessCreationFailed);
            VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::ProcessCreation);
            VERIFY_ARE_EQUAL(DWORD{ ERROR_ACCESS_DENIED }, result.SystemError);
            VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.PrimaryThread));
            VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.Process));
            VERIFY_IS_TRUE(api->TerminateHandles.empty());
        }
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            api->CreatedProcess.PrimaryThread = nullptr;
            const auto launcher = MakeTestProcessLauncher(api);
            const auto process = api->CreatedProcess.Process;
            const auto result = launcher->Launch(ValidLaunchRequest(identity));
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::ProcessCreationFailed);
            VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, process));
            VERIFY_ARE_EQUAL(size_t{ 0 }, api->ResumeHandles.size());
        }
    }

    void RuntimeProcessLauncherFailureTests::ChildJobQueryFailureAndStillInJobRollbackBeforeResume()
    {
        UniqueActivationIdentity identity;
        for (const auto queryFailed : { false, true })
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            api->CurrentProcessIsInJob = !queryFailed;
            api->CurrentJobLimits = queryFailed ? 0 : JOB_OBJECT_LIMIT_BREAKAWAY_OK;
            api->ChildJobQuerySucceeded = !queryFailed;
            api->ChildProcessIsInJob = !queryFailed;
            api->ChildJobQueryError = queryFailed ? ERROR_INVALID_HANDLE : ERROR_SUCCESS;
            const auto launcher = MakeTestProcessLauncher(api);
            const auto result = launcher->Launch(ValidLaunchRequest(identity));
            VERIFY_IS_TRUE(result.Status == (queryFailed ?
                                                  RuntimeProcessLaunchStatus::ChildJobQueryFailed :
                                                  RuntimeProcessLaunchStatus::ChildStillInJob));
            VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::ChildJobVerification);
            VERIFY_IS_TRUE(result.JobStrategy == (queryFailed ?
                                                       RuntimeParentJobStrategy::NotInJob :
                                                       RuntimeParentJobStrategy::ExplicitBreakaway));
            VERIFY_ARE_EQUAL(size_t{ 1 }, api->TerminateHandles.size());
            VERIFY_ARE_EQUAL(api->CreatedProcess.Process, api->TerminateHandles.front());
            VERIFY_ARE_EQUAL(size_t{ 1 }, api->WaitProcessHandles.size());
            VERIFY_ARE_EQUAL(DWORD{ 5000 }, api->WaitProcessTimeouts.front());
            VERIFY_IS_TRUE(api->ResumeHandles.empty());
            VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.PrimaryThread));
            VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.Process));
        }
    }

    void RuntimeProcessLauncherFailureTests::ResumeFailureRollsBackSuspendedProcess()
    {
        UniqueActivationIdentity identity;
        for (const auto wrongSuspendCount : { false, true })
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            api->ResumeSucceeded = wrongSuspendCount;
            api->ResumePreviousSuspendCount = wrongSuspendCount ?
                                                  2 :
                                                  static_cast<DWORD>(-1);
            api->ResumeError = wrongSuspendCount ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
            const auto launcher = MakeTestProcessLauncher(api);
            const auto result = launcher->Launch(ValidLaunchRequest(identity));
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::ResumeFailed);
            VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::Resume);
            VERIFY_ARE_EQUAL(
                wrongSuspendCount ? DWORD{ ERROR_INVALID_STATE } : DWORD{ ERROR_ACCESS_DENIED },
                result.SystemError);
            VERIFY_ARE_EQUAL(size_t{ 1 }, api->ResumeHandles.size());
            VERIFY_ARE_EQUAL(size_t{ 1 }, api->TerminateHandles.size());
            VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.PrimaryThread));
            VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.Process));
        }
    }

    void RuntimeProcessLauncherFailureTests::RollbackFailureDominatesOriginalFailureAndStillClosesHandles()
    {
        UniqueActivationIdentity identity;
        const std::array firstWaits{
            RuntimeWindowsWaitResult{
                RuntimeWindowsWaitStatus::NotSignaled,
                ERROR_SUCCESS,
            },
            RuntimeWindowsWaitResult{
                RuntimeWindowsWaitStatus::Failed,
                ERROR_INVALID_HANDLE,
            },
        };
        for (const auto firstWait : firstWaits)
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            api->ChildJobQuerySucceeded = false;
            api->ChildJobQueryError = ERROR_INVALID_HANDLE;
            api->TerminateSucceeded = true;
            api->WaitProcessResults.emplace_back(firstWait);
            api->WaitProcessResults.emplace_back(RuntimeWindowsWaitResult{
                RuntimeWindowsWaitStatus::Signaled,
                ERROR_SUCCESS,
            });
            const auto launcher = MakeTestProcessLauncher(api);
            const auto result = launcher->Launch(ValidLaunchRequest(identity));
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::RollbackFailed);
            VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::Rollback);
            VERIFY_ARE_EQUAL(
                firstWait.Status == RuntimeWindowsWaitStatus::Failed ?
                    DWORD{ ERROR_INVALID_HANDLE } :
                    DWORD{ WAIT_TIMEOUT },
                result.SystemError);
            VERIFY_IS_TRUE(WaitUntil([&] {
                return HandleCloseCount(*api, api->CreatedProcess.Process) == 1;
            }));
            VERIFY_IS_TRUE(api->TerminateHandles.size() >= 2);
            VERIFY_IS_TRUE(api->WaitProcessHandles.size() >= 2);
            VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.PrimaryThread));
            VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.Process));
        }
    }
}
