// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeLaunchingConnectorTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeLaunchingConnectorFlowTests
    {
        TEST_CLASS(RuntimeLaunchingConnectorFlowTests);

        TEST_METHOD(InvalidConfigurationFailsBeforeTouchingDependencies);
        TEST_METHOD(ExistingRuntimeSkipsLockAndLaunch);
        TEST_METHOD(AbsentRuntimeLaunchesOnceAndCompletesHello);
        TEST_METHOD(RuntimeAppearingBeforeLockedReprobeSkipsLaunch);
        TEST_METHOD(BusyOrHandshakingEndpointNeverLaunches);
        TEST_METHOD(LockTimeoutAndCancellationDoNotLaunch);
        TEST_METHOD(AllStagesShareOneAbsoluteDeadline);
        TEST_METHOD(PostLaunchCancellationClosesObservationWithoutTermination);
        TEST_METHOD(PostLaunchDeadlineAndWaitFailuresCloseObservationWithoutTermination);
    };

    void RuntimeLaunchingConnectorFlowTests::InvalidConfigurationFailsBeforeTouchingDependencies()
    {
        LaunchingFixture fixture;
        const auto zeroTimeout = fixture.Connect(0);
        const auto oversizedTimeout = fixture.Connect(
            MaximumRuntimeTransportTimeoutMilliseconds + 1);
        RuntimeLaunchingControlConnector invalidConfiguration{
            {
                LaunchingConfiguration().RuntimeExecutable,
                LaunchingConfiguration().TrustedRoot,
                0,
            },
            {
                fixture.Direct,
                fixture.Locks,
                fixture.Launcher,
                fixture.Services,
            },
        };
        const auto invalidPoll = invalidConfiguration.Connect(
            fixture.Identity.Identity(),
            LaunchingHello(fixture.Identity.Identity()),
            1000);

        const std::array results{ &zeroTimeout, &oversizedTimeout, &invalidPoll };
        for (const auto result : results)
        {
            VERIFY_IS_TRUE(result->Status == RuntimeTransportStatus::InvalidConfiguration);
            VERIFY_IS_TRUE(
                result->FailureReason ==
                RuntimeControlConnectFailureReason::InvalidConfiguration);
            VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_PARAMETER }, result->SystemError);
        }
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Direct->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Locks->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Launcher->Calls.load());
    }

    void RuntimeLaunchingConnectorFlowTests::ExistingRuntimeSkipsLockAndLaunch()
    {
        LaunchingFixture fixture;
        fixture.Direct->Enqueue(ConnectedResult());

        const auto result = fixture.Connect();

        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Direct->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Locks->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Launcher->Calls.load());
    }

    void RuntimeLaunchingConnectorFlowTests::AbsentRuntimeLaunchesOnceAndCompletesHello()
    {
        LaunchingFixture fixture;
        fixture.Direct->Enqueue(AbsentResult());
        fixture.Direct->Enqueue(AbsentResult());
        fixture.Direct->Enqueue(AbsentResult());
        fixture.Direct->Enqueue(ConnectedResult());
        const auto processApi = ConfigureSuccessfulProcessLauncher(fixture);
        fixture.Services->AdvanceToDeadlineOnTimeout = true;
        fixture.Services->Enqueue({
            RuntimeActivationWaitStatus::DeadlineReached,
            InvalidRuntimeActivationWaitIndex,
            ERROR_SUCCESS,
        });

        const auto result = fixture.Connect();

        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 4 }, fixture.Direct->Calls.load());
        VERIFY_IS_TRUE((
            fixture.Direct->Timeouts ==
            std::vector<uint32_t>{ 1000, 1000, 1000, 950 }));
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Launcher->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Locks->State->Active.load());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->State->Released.load());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Launcher->Requests.size());
        if (!fixture.Launcher->Requests.empty())
        {
            const auto& request = fixture.Launcher->Requests.front();
            VERIFY_ARE_EQUAL(
                LaunchingConfiguration().RuntimeExecutable.native(),
                request.ExecutablePath.native());
            VERIFY_ARE_EQUAL(
                LaunchingConfiguration().TrustedRoot.native(),
                request.TrustedRoot.native());
            VERIFY_ARE_EQUAL(
                fixture.Identity.Identity().ProtocolIdentity,
                request.Identity.ProtocolIdentity);
        }
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            HandleCloseCount(*processApi, processApi->CreatedProcess.Process));
        VERIFY_IS_TRUE(processApi->TerminateHandles.empty());
    }

    void RuntimeLaunchingConnectorFlowTests::RuntimeAppearingBeforeLockedReprobeSkipsLaunch()
    {
        LaunchingFixture fixture;
        fixture.Direct->Enqueue(AbsentResult());
        fixture.Direct->Enqueue(ConnectedResult());

        const auto result = fixture.Connect();

        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 2 }, fixture.Direct->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Launcher->Calls.load());
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Locks->State->Active.load());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->State->Released.load());
    }

    void RuntimeLaunchingConnectorFlowTests::BusyOrHandshakingEndpointNeverLaunches()
    {
        const std::array failures{
            FailureResult(
                RuntimeTransportStatus::Timeout,
                RuntimeControlConnectFailureReason::EndpointBusy),
            FailureResult(
                RuntimeTransportStatus::ProtocolRejected,
                RuntimeControlConnectFailureReason::HandshakeRejected,
                RuntimeControlConnectPhase::Handshaking),
            FailureResult(
                RuntimeTransportStatus::MalformedFrame,
                RuntimeControlConnectFailureReason::HandshakeMalformed,
                RuntimeControlConnectPhase::Handshaking),
        };

        for (const auto& failure : failures)
        {
            LaunchingFixture fixture;
            fixture.Direct->Enqueue(FailureResult(
                failure.Status,
                failure.FailureReason,
                failure.Phase));

            const auto result = fixture.Connect();

            VERIFY_IS_TRUE(result.FailureReason == failure.FailureReason);
            VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Locks->Calls.load());
            VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Launcher->Calls.load());
        }
    }

    void RuntimeLaunchingConnectorFlowTests::LockTimeoutAndCancellationDoNotLaunch()
    {
        struct LockCase
        {
            RuntimeLaunchLockStatus LockStatus;
            RuntimeTransportStatus ExpectedStatus;
            RuntimeControlConnectFailureReason ExpectedReason;
            DWORD SystemError;
        };
        constexpr std::array cases{
            LockCase{
                RuntimeLaunchLockStatus::TimedOut,
                RuntimeTransportStatus::Timeout,
                RuntimeControlConnectFailureReason::LaunchLockTimeout,
                WAIT_TIMEOUT,
            },
            LockCase{
                RuntimeLaunchLockStatus::Cancelled,
                RuntimeTransportStatus::Cancelled,
                RuntimeControlConnectFailureReason::Cancelled,
                ERROR_OPERATION_ABORTED,
            },
        };

        for (const auto& test : cases)
        {
            LaunchingFixture fixture;
            auto cancellation = MakeTestEvent();
            fixture.Direct->Enqueue(AbsentResult());
            fixture.Locks->Status = test.LockStatus;
            fixture.Locks->SystemError = test.SystemError;

            const auto result = fixture.Connect(1000, cancellation.get());

            VERIFY_IS_TRUE(result.Status == test.ExpectedStatus);
            VERIFY_IS_TRUE(result.FailureReason == test.ExpectedReason);
            VERIFY_ARE_EQUAL(test.SystemError, result.SystemError);
            VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->Calls.load());
            VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Launcher->Calls.load());
            VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->CancellationEvents.size());
            if (!fixture.Locks->CancellationEvents.empty())
            {
                VERIFY_ARE_EQUAL(
                    cancellation.get(),
                    fixture.Locks->CancellationEvents.front());
            }
        }
    }

    void RuntimeLaunchingConnectorFlowTests::AllStagesShareOneAbsoluteDeadline()
    {
        LaunchingFixture fixture;
        std::atomic_size_t directCall{};
        fixture.Direct->SetHandler([&] {
            const auto call = directCall.fetch_add(1, std::memory_order_relaxed);
            if (call == 0)
            {
                fixture.Services->SetNow(150);
                return AbsentResult();
            }
            if (call == 1)
            {
                fixture.Services->SetNow(200);
                return AbsentResult();
            }
            if (call == 2)
            {
                fixture.Services->SetNow(300);
                return AbsentResult();
            }
            return ConnectedResult();
        });
        const auto processApi = std::make_shared<FakeRuntimeWindowsProcessApi>();
        const auto concreteLauncher = MakeTestProcessLauncher(processApi);
        fixture.Launcher->Handler = [&](const RuntimeProcessLaunchRequest& request) {
            fixture.Services->SetNow(250);
            return concreteLauncher->Launch(request);
        };
        fixture.Services->AdvanceToDeadlineOnTimeout = true;
        fixture.Services->Enqueue({
            RuntimeActivationWaitStatus::DeadlineReached,
            InvalidRuntimeActivationWaitIndex,
            ERROR_SUCCESS,
        });

        const auto result = fixture.Connect();

        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE((
            fixture.Direct->Timeouts ==
            std::vector<uint32_t>{ 1000, 950, 850, 750 }));
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->Deadlines.size());
        if (!fixture.Locks->Deadlines.empty())
        {
            VERIFY_ARE_EQUAL(
                RuntimeActivationMonotonicMilliseconds{ 1100 },
                fixture.Locks->Deadlines.front().Value);
        }
        const auto waits = fixture.Services->Deadlines();
        VERIFY_ARE_EQUAL(size_t{ 1 }, waits.size());
        if (!waits.empty())
        {
            VERIFY_ARE_EQUAL(
                RuntimeActivationMonotonicMilliseconds{ 350 },
                waits.front().Value);
        }
        VERIFY_IS_TRUE(processApi->TerminateHandles.empty());
    }

    void RuntimeLaunchingConnectorFlowTests::PostLaunchCancellationClosesObservationWithoutTermination()
    {
        LaunchingFixture fixture;
        auto cancellation = MakeTestEvent();
        fixture.Direct->Enqueue(AbsentResult());
        fixture.Direct->Enqueue(AbsentResult());
        fixture.Direct->Enqueue(AbsentResult());
        const auto processApi = ConfigureSuccessfulProcessLauncher(fixture);
        fixture.Services->Enqueue({
            RuntimeActivationWaitStatus::WakeSignaled,
            0,
            ERROR_SUCCESS,
        });

        const auto result = fixture.Connect(1000, cancellation.get());

        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::Cancelled);
        VERIFY_IS_TRUE(result.FailureReason == RuntimeControlConnectFailureReason::Cancelled);
        VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Locks->State->Active.load());
        VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->State->Released.load());
        const auto wakeHandles = fixture.Services->WakeHandles();
        VERIFY_ARE_EQUAL(size_t{ 1 }, wakeHandles.size());
        if (!wakeHandles.empty())
        {
            VERIFY_ARE_EQUAL(size_t{ 2 }, wakeHandles.front().size());
            if (wakeHandles.front().size() == 2)
            {
                VERIFY_ARE_EQUAL(cancellation.get(), wakeHandles.front()[0]);
                VERIFY_ARE_EQUAL(processApi->CreatedProcess.Process, wakeHandles.front()[1]);
            }
        }
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            HandleCloseCount(*processApi, processApi->CreatedProcess.Process));
        VERIFY_IS_TRUE(processApi->TerminateHandles.empty());
    }

    void RuntimeLaunchingConnectorFlowTests::PostLaunchDeadlineAndWaitFailuresCloseObservationWithoutTermination()
    {
        struct WaitCase
        {
            RuntimeActivationWaitStatus WaitStatus;
            DWORD SystemError;
            RuntimeTransportStatus ExpectedStatus;
            RuntimeControlConnectFailureReason ExpectedReason;
            uint32_t Timeout;
        };
        constexpr std::array cases{
            WaitCase{
                RuntimeActivationWaitStatus::DeadlineReached,
                ERROR_SUCCESS,
                RuntimeTransportStatus::Timeout,
                RuntimeControlConnectFailureReason::DeadlineExpired,
                50,
            },
            WaitCase{
                RuntimeActivationWaitStatus::Failed,
                ERROR_INVALID_HANDLE,
                RuntimeTransportStatus::SystemError,
                RuntimeControlConnectFailureReason::ActivationWaitFailed,
                1000,
            },
            WaitCase{
                RuntimeActivationWaitStatus::WakeAbandoned,
                ERROR_ABANDONED_WAIT_0,
                RuntimeTransportStatus::SystemError,
                RuntimeControlConnectFailureReason::ActivationWaitFailed,
                1000,
            },
        };

        for (const auto& test : cases)
        {
            LaunchingFixture fixture;
            fixture.Direct->Enqueue(AbsentResult());
            fixture.Direct->Enqueue(AbsentResult());
            fixture.Direct->Enqueue(AbsentResult());
            const auto processApi = ConfigureSuccessfulProcessLauncher(fixture);
            fixture.Services->AdvanceToDeadlineOnTimeout = true;
            fixture.Services->Enqueue({
                test.WaitStatus,
                InvalidRuntimeActivationWaitIndex,
                test.SystemError,
            });

            const auto result = fixture.Connect(test.Timeout);

            VERIFY_IS_TRUE(result.Status == test.ExpectedStatus);
            VERIFY_IS_TRUE(result.FailureReason == test.ExpectedReason);
            VERIFY_ARE_EQUAL(size_t{ 0 }, fixture.Locks->State->Active.load());
            VERIFY_ARE_EQUAL(size_t{ 1 }, fixture.Locks->State->Released.load());
            VERIFY_ARE_EQUAL(
                size_t{ 1 },
                HandleCloseCount(*processApi, processApi->CreatedProcess.Process));
            VERIFY_IS_TRUE(processApi->TerminateHandles.empty());
        }
    }
}
