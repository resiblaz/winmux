// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeDirectConnectDisposition.h"
#include "RuntimeLaunchingConnectorTestSupport.h"
#include "RuntimeProcessExitCode.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    namespace
    {
        std::shared_ptr<FakeRuntimeWindowsProcessApi> _configureExitedProcess(
            LaunchingFixture& fixture,
            const DWORD exitCode)
        {
            const auto api = ConfigureSuccessfulProcessLauncher(fixture);
            api->DefaultPollResult = {
                RuntimeWindowsWaitStatus::Signaled,
                ERROR_SUCCESS,
            };
            api->ConfiguredExitCode = exitCode;
            return api;
        }

        void _enqueueLaunchPath(LaunchingFixture& fixture)
        {
            fixture.Direct->Enqueue(AbsentResult());
            fixture.Direct->Enqueue(AbsentResult());
            fixture.Direct->Enqueue(AbsentResult());
        }
    }

    class RuntimeLaunchingConnectorExitTests
    {
        TEST_CLASS(RuntimeLaunchingConnectorExitTests);

        TEST_METHOD(LaunchFailureIsTypedAndAlwaysReleasesLease);
        TEST_METHOD(DeterministicRuntimeExitCodesAreLaunchFailures);
        TEST_METHOD(UnexpectedRuntimeExitIsRetryableWithoutRelaunch);
        TEST_METHOD(SecondInstanceExitKeepsWaitingForWinner);
        TEST_METHOD(ProcessObservationFailureIsTyped);
    };

    void RuntimeLaunchingConnectorExitTests::LaunchFailureIsTypedAndAlwaysReleasesLease()
    {
        struct LaunchCase
        {
            RuntimeProcessLaunchStatus LaunchStatus;
            RuntimeTransportStatus ExpectedStatus;
            RuntimeControlConnectFailureReason ExpectedReason;
        };
        constexpr std::array cases{
            LaunchCase{
                RuntimeProcessLaunchStatus::CancelledBeforeCreate,
                RuntimeTransportStatus::Cancelled,
                RuntimeControlConnectFailureReason::Cancelled,
            },
            LaunchCase{
                RuntimeProcessLaunchStatus::InvalidExecutablePath,
                RuntimeTransportStatus::InvalidConfiguration,
                RuntimeControlConnectFailureReason::LaunchFailed,
            },
            LaunchCase{
                RuntimeProcessLaunchStatus::ProcessCreationFailed,
                RuntimeTransportStatus::SystemError,
                RuntimeControlConnectFailureReason::LaunchFailed,
            },
        };

        for (const auto& test : cases)
        {
            LaunchingFixture fixture;
            fixture.Direct->Enqueue(AbsentResult());
            fixture.Direct->Enqueue(AbsentResult());
            fixture.Launcher->Handler = [&](const RuntimeProcessLaunchRequest&) {
                RuntimeProcessLaunchResult launched;
                launched.Status = test.LaunchStatus;
                launched.SystemError = ERROR_BAD_EXE_FORMAT;
                launched.Error = "injected launch failure";
                return launched;
            };

            const auto result = fixture.Connect();

            VERIFY_IS_TRUE(result.Status == test.ExpectedStatus);
            VERIFY_IS_TRUE(result.FailureReason == test.ExpectedReason);
            VERIFY_ARE_EQUAL(DWORD{ ERROR_BAD_EXE_FORMAT }, result.SystemError);
            VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Launcher->Calls.load());
            VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Locks->State->Active.load());
            VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->State->Released.load());
        }
    }

    void RuntimeLaunchingConnectorExitTests::DeterministicRuntimeExitCodesAreLaunchFailures()
    {
        constexpr std::array exitCodes{
            RuntimeProcessExitCode::BootstrapFailure,
            RuntimeProcessExitCode::InvalidCommandLine,
            RuntimeProcessExitCode::RuntimeInstanceIdFailure,
            RuntimeProcessExitCode::ServerStartFailure,
            RuntimeProcessExitCode::StopInfrastructureFailure,
        };
        for (const auto exitCode : exitCodes)
        {
            LaunchingFixture fixture;
            _enqueueLaunchPath(fixture);
            const auto rawExitCode = static_cast<DWORD>(exitCode);
            const auto processApi = _configureExitedProcess(fixture, rawExitCode);

            const auto result = fixture.Connect();

            VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::InvalidConfiguration);
            VERIFY_IS_TRUE(result.FailureReason == RuntimeControlConnectFailureReason::LaunchFailed);
            VERIFY_IS_TRUE(result.LaunchedProcessExitCode.has_value());
            if (result.LaunchedProcessExitCode)
            {
                VERIFY_ARE_EQUAL(rawExitCode, *result.LaunchedProcessExitCode);
            }
            VERIFY_ARE_EQUAL(size_t{ 1 }, processApi->ExitCodeHandles.size());
            VERIFY_ARE_EQUAL(
                size_t{ 1 },
                HandleCloseCount(*processApi, processApi->CreatedProcess.Process));
            VERIFY_IS_TRUE(processApi->TerminateHandles.empty());
        }
    }

    void RuntimeLaunchingConnectorExitTests::UnexpectedRuntimeExitIsRetryableWithoutRelaunch()
    {
        LaunchingFixture fixture;
        _enqueueLaunchPath(fixture);
        const auto processApi = _configureExitedProcess(fixture, 42);

        const auto result = fixture.Connect();

        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::Disconnected);
        VERIFY_IS_TRUE(
            result.FailureReason == RuntimeControlConnectFailureReason::LaunchedProcessExited);
        VERIFY_IS_TRUE(
            EvaluateRuntimeDirectConnectDisposition(result) ==
            RuntimeDirectConnectDisposition::RetryConnectOnly);
        VERIFY_IS_TRUE(result.LaunchedProcessExitCode.has_value());
        if (result.LaunchedProcessExitCode)
        {
            VERIFY_ARE_EQUAL(DWORD{ 42 }, *result.LaunchedProcessExitCode);
        }
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Launcher->Calls.load());
        VERIFY_IS_TRUE(processApi->TerminateHandles.empty());
    }

    void RuntimeLaunchingConnectorExitTests::SecondInstanceExitKeepsWaitingForWinner()
    {
        LaunchingFixture fixture;
        _enqueueLaunchPath(fixture);
        fixture.Direct->Enqueue(ConnectedResult());
        const auto processApi = _configureExitedProcess(
            fixture,
            static_cast<DWORD>(RuntimeProcessExitCode::AlreadyRunning));
        fixture.Services->AdvanceToDeadlineOnTimeout = true;
        fixture.Services->Enqueue({
            RuntimeActivationWaitStatus::DeadlineReached,
            InvalidRuntimeActivationWaitIndex,
            ERROR_SUCCESS,
        });

        const auto result = fixture.Connect();

        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Launcher->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 4 }, fixture.Direct->Calls.load());
        const auto wakeHandles = fixture.Services->WakeHandles();
        VERIFY_ARE_EQUAL(size_t{ 1 }, wakeHandles.size());
        if (!wakeHandles.empty())
        {
            VERIFY_IS_TRUE(wakeHandles.front().empty());
        }
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            HandleCloseCount(*processApi, processApi->CreatedProcess.Process));
        VERIFY_IS_TRUE(processApi->TerminateHandles.empty());
    }

    void RuntimeLaunchingConnectorExitTests::ProcessObservationFailureIsTyped()
    {
        LaunchingFixture fixture;
        _enqueueLaunchPath(fixture);
        const auto processApi = ConfigureSuccessfulProcessLauncher(fixture);
        processApi->DefaultPollResult = {
            RuntimeWindowsWaitStatus::Failed,
            ERROR_INVALID_HANDLE,
        };

        const auto result = fixture.Connect();

        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::SystemError);
        VERIFY_IS_TRUE(
            result.FailureReason == RuntimeControlConnectFailureReason::ActivationWaitFailed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, result.SystemError);
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            HandleCloseCount(*processApi, processApi->CreatedProcess.Process));
        VERIFY_IS_TRUE(processApi->TerminateHandles.empty());
    }
}
