// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeProcessLauncherTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeProcessObservationTests
    {
        TEST_CLASS(RuntimeProcessObservationTests);

        TEST_METHOD(ReportsRunningThenExitedWithExactProcessIdentity);
        TEST_METHOD(PollExitQueryAndStillActiveFailuresAreTyped);
        TEST_METHOD(CloseIsIdempotentAndNeverTerminatesProcess);
    };

    void RuntimeProcessObservationTests::ReportsRunningThenExitedWithExactProcessIdentity()
    {
        UniqueActivationIdentity identity;
        auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
        const auto launcher = MakeTestProcessLauncher(api);
        auto launched = launcher->Launch(ValidLaunchRequest(identity));
        VERIFY_IS_TRUE(launched.Started());
        VERIFY_ARE_EQUAL(api->CreatedProcess.Process, launched.Process->WaitHandle());
        VERIFY_ARE_EQUAL(api->CreatedProcess.ProcessId, launched.Process->ProcessId());

        auto exit = launched.Process->QueryExit();
        VERIFY_IS_TRUE(exit.Status == RuntimeProcessExitStatus::Running);
        VERIFY_ARE_EQUAL(DWORD{ STILL_ACTIVE }, exit.ExitCode);

        api->DefaultPollResult = {
            RuntimeWindowsWaitStatus::Signaled,
            ERROR_SUCCESS,
        };
        api->ConfiguredExitCode = 42;
        exit = launched.Process->QueryExit();
        VERIFY_IS_TRUE(exit.Status == RuntimeProcessExitStatus::Exited);
        VERIFY_ARE_EQUAL(DWORD{ 42 }, exit.ExitCode);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_SUCCESS }, exit.SystemError);
        VERIFY_ARE_EQUAL(size_t{ 1 }, api->ExitCodeHandles.size());
        VERIFY_ARE_EQUAL(api->CreatedProcess.Process, api->ExitCodeHandles.front());
    }

    void RuntimeProcessObservationTests::PollExitQueryAndStillActiveFailuresAreTyped()
    {
        UniqueActivationIdentity identity;
        auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
        const auto launcher = MakeTestProcessLauncher(api);
        auto launched = launcher->Launch(ValidLaunchRequest(identity));
        VERIFY_IS_TRUE(launched.Started());

        api->DefaultPollResult = {
            RuntimeWindowsWaitStatus::Failed,
            ERROR_INVALID_HANDLE,
        };
        auto exit = launched.Process->QueryExit();
        VERIFY_IS_TRUE(exit.Status == RuntimeProcessExitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, exit.SystemError);

        api->DefaultPollResult = {
            RuntimeWindowsWaitStatus::Signaled,
            ERROR_SUCCESS,
        };
        api->ExitCodeSucceeded = false;
        api->ExitCodeError = ERROR_ACCESS_DENIED;
        exit = launched.Process->QueryExit();
        VERIFY_IS_TRUE(exit.Status == RuntimeProcessExitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_ACCESS_DENIED }, exit.SystemError);

        api->ExitCodeSucceeded = true;
        api->ExitCodeError = ERROR_SUCCESS;
        api->ConfiguredExitCode = STILL_ACTIVE;
        exit = launched.Process->QueryExit();
        VERIFY_IS_TRUE(exit.Status == RuntimeProcessExitStatus::Failed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_STATE }, exit.SystemError);
    }

    void RuntimeProcessObservationTests::CloseIsIdempotentAndNeverTerminatesProcess()
    {
        UniqueActivationIdentity identity;
        auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
        const auto launcher = MakeTestProcessLauncher(api);
        auto launched = launcher->Launch(ValidLaunchRequest(identity));
        VERIFY_IS_TRUE(launched.Started());

        launched.Process->Close();
        launched.Process->Close();
        const auto exit = launched.Process->QueryExit();
        VERIFY_IS_TRUE(exit.Status == RuntimeProcessExitStatus::Closed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, exit.SystemError);
        VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.Process));
        VERIFY_IS_TRUE(api->TerminateHandles.empty());
    }
}
