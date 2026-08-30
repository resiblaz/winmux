// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeTerminalLifecycle.h"

#include <limits>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        RuntimeTerminalLifecycleState _ready(
            const uint64_t generation = 7,
            const uint32_t idleMilliseconds = 100)
        {
            return RuntimeTerminalLifecycleState{
                {
                    TaskTerminalState::Ready,
                    generation,
                    idleMilliseconds,
                }
            };
        }
    }

    class RuntimeTerminalLifecycleTests
    {
        TEST_CLASS(RuntimeTerminalLifecycleTests);

        TEST_METHOD(DefaultGraceIsFiveMinutesAndExpirySuspends);
        TEST_METHOD(ActiveRunCancelsAndRestartsTheFullGrace);
        TEST_METHOD(AttachedClientWinsTheDeadlineRace);
        TEST_METHOD(DeadlineWinsTheAttachRaceAndWakeUsesNewGeneration);
        TEST_METHOD(NewWorkWakesSuspendedAndPassiveReconnectDoesNot);
        TEST_METHOD(ClosedNeverAutomaticallyWakes);
        TEST_METHOD(WakeFailureIsRecordedAndCanStartAnotherGeneration);
        TEST_METHOD(SynchronizationRejectsStaleGenerationAndRearmsReady);
        TEST_METHOD(ClockAndGenerationExhaustionFailClosed);
        TEST_METHOD(InvalidInitialStateGenerationAndDurationAreRejected);
    };

    void RuntimeTerminalLifecycleTests::DefaultGraceIsFiveMinutesAndExpirySuspends()
    {
        VERIFY_ARE_EQUAL(uint32_t{ 300000 }, DefaultRuntimeTerminalIdleSuspendMilliseconds);
        RuntimeTerminalLifecycleState lifecycle{
            {
                TaskTerminalState::Ready,
                3,
                DefaultRuntimeTerminalIdleSuspendMilliseconds,
            }
        };
        VERIFY_IS_TRUE(lifecycle.Valid());

        const auto armed = lifecycle.Observe(false, 0, 1000);
        VERIFY_IS_TRUE(armed.Status == RuntimeTerminalLifecycleStatus::IdleGraceStarted);
        VERIFY_IS_TRUE(armed.Action == RuntimeTerminalLifecycleAction::Wait);
        VERIFY_IS_TRUE(armed.Deadline == std::optional<uint64_t>{ 301000 });
        VERIFY_IS_TRUE(
            lifecycle.Observe(false, 0, 300999).Status ==
            RuntimeTerminalLifecycleStatus::IdleGraceContinues);

        const auto suspended = lifecycle.Observe(false, 0, 301000);
        VERIFY_IS_TRUE(suspended.Status == RuntimeTerminalLifecycleStatus::Suspended);
        VERIFY_IS_TRUE(
            suspended.Action == RuntimeTerminalLifecycleAction::SuspendProjection);
        VERIFY_IS_TRUE(suspended.State == TaskTerminalState::Suspended);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, suspended.Generation);
        VERIFY_IS_FALSE(suspended.Deadline.has_value());
    }

    void RuntimeTerminalLifecycleTests::ActiveRunCancelsAndRestartsTheFullGrace()
    {
        auto lifecycle = _ready();
        VERIFY_IS_TRUE(
            lifecycle.Observe(false, 0, 10).Deadline ==
            std::optional<uint64_t>{ 110 });

        const auto active = lifecycle.Observe(true, 0, 110);
        VERIFY_IS_TRUE(active.Status == RuntimeTerminalLifecycleStatus::Live);
        VERIFY_IS_FALSE(active.Deadline.has_value());
        VERIFY_IS_TRUE(lifecycle.Snapshot().HasActiveRun);

        const auto restarted = lifecycle.Observe(false, 0, 150);
        VERIFY_IS_TRUE(
            restarted.Status == RuntimeTerminalLifecycleStatus::IdleGraceStarted);
        VERIFY_IS_TRUE(restarted.Deadline == std::optional<uint64_t>{ 250 });
        VERIFY_IS_TRUE(
            lifecycle.Observe(false, 0, 249).Status ==
            RuntimeTerminalLifecycleStatus::IdleGraceContinues);
        VERIFY_IS_TRUE(
            lifecycle.Observe(false, 0, 250).Action ==
            RuntimeTerminalLifecycleAction::SuspendProjection);
    }

    void RuntimeTerminalLifecycleTests::AttachedClientWinsTheDeadlineRace()
    {
        auto lifecycle = _ready();
        VERIFY_IS_TRUE(
            lifecycle.Observe(false, 0, 10).Deadline ==
            std::optional<uint64_t>{ 110 });

        // The authoritative attach observation is serialized before the due
        // timer evaluation, so the client wins even at the exact deadline.
        const auto attached = lifecycle.Observe(false, 1, 110);
        VERIFY_IS_TRUE(attached.Status == RuntimeTerminalLifecycleStatus::Live);
        VERIFY_IS_TRUE(attached.State == TaskTerminalState::Ready);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, attached.Generation);
        VERIFY_IS_FALSE(attached.Deadline.has_value());
        VERIFY_ARE_EQUAL(size_t{ 1 }, lifecycle.Snapshot().AttachedClients);

        const auto reconnect = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::PassiveReconnect,
            110);
        VERIFY_IS_TRUE(
            reconnect.Status == RuntimeTerminalLifecycleStatus::AlreadyAwake);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, reconnect.Generation);

        const auto detached = lifecycle.Observe(false, 0, 120);
        VERIFY_IS_TRUE(detached.Deadline == std::optional<uint64_t>{ 220 });
    }

    void RuntimeTerminalLifecycleTests::DeadlineWinsTheAttachRaceAndWakeUsesNewGeneration()
    {
        auto lifecycle = _ready();
        (void)lifecycle.Observe(false, 0, 10);
        const auto suspended = lifecycle.Observe(false, 0, 110);
        VERIFY_IS_TRUE(
            suspended.Action == RuntimeTerminalLifecycleAction::SuspendProjection);

        const auto wake = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::ExplicitOpen,
            110);
        VERIFY_IS_TRUE(wake.Status == RuntimeTerminalLifecycleStatus::WakeStarted);
        VERIFY_IS_TRUE(wake.Action == RuntimeTerminalLifecycleAction::StartProjection);
        VERIFY_ARE_EQUAL(uint64_t{ 8 }, wake.Generation);
        VERIFY_IS_TRUE(wake.State == TaskTerminalState::Starting);

        const auto concurrentWork = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::NewWork,
            110);
        VERIFY_IS_TRUE(
            concurrentWork.Status == RuntimeTerminalLifecycleStatus::WakeReplayed);
        VERIFY_ARE_EQUAL(wake.Generation, concurrentWork.Generation);
        VERIFY_IS_TRUE(concurrentWork.Action == RuntimeTerminalLifecycleAction::None);

        const auto ready = lifecycle.CompleteWake(8, true, 111);
        VERIFY_IS_TRUE(ready.Status == RuntimeTerminalLifecycleStatus::WakeCompleted);
        VERIFY_IS_TRUE(ready.State == TaskTerminalState::Ready);
        const auto attached = lifecycle.Observe(false, 1, 111);
        VERIFY_IS_TRUE(attached.Status == RuntimeTerminalLifecycleStatus::Live);
        VERIFY_ARE_EQUAL(uint64_t{ 8 }, attached.Generation);
    }

    void RuntimeTerminalLifecycleTests::NewWorkWakesSuspendedAndPassiveReconnectDoesNot()
    {
        RuntimeTerminalLifecycleState lifecycle{
            { TaskTerminalState::Suspended, 4, 100 }
        };
        const auto reconnect = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::PassiveReconnect,
            10);
        VERIFY_IS_TRUE(reconnect.Status == RuntimeTerminalLifecycleStatus::WakeRejected);
        VERIFY_IS_TRUE(reconnect.State == TaskTerminalState::Suspended);
        VERIFY_ARE_EQUAL(uint64_t{ 4 }, reconnect.Generation);

        const auto work = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::NewWork,
            11);
        VERIFY_IS_TRUE(work.Status == RuntimeTerminalLifecycleStatus::WakeStarted);
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, work.Generation);
        VERIFY_IS_TRUE(work.Action == RuntimeTerminalLifecycleAction::StartProjection);
    }

    void RuntimeTerminalLifecycleTests::ClosedNeverAutomaticallyWakes()
    {
        auto lifecycle = _ready(5);
        VERIFY_IS_TRUE(
            lifecycle.Observe(false, 1, 10).Status ==
            RuntimeTerminalLifecycleStatus::Live);
        const auto closed = lifecycle.Synchronize(
            TaskTerminalState::Closed,
            5,
            11);
        VERIFY_IS_TRUE(closed.Status == RuntimeTerminalLifecycleStatus::Synchronized);
        VERIFY_IS_TRUE(closed.State == TaskTerminalState::Closed);

        const auto observed = lifecycle.Observe(true, 1, 12);
        VERIFY_IS_TRUE(observed.Status == RuntimeTerminalLifecycleStatus::WakeRejected);
        VERIFY_IS_TRUE(observed.State == TaskTerminalState::Closed);
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, observed.Generation);

        const auto work = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::NewWork,
            13);
        VERIFY_IS_TRUE(work.Status == RuntimeTerminalLifecycleStatus::WakeRejected);
        const auto reconnect = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::PassiveReconnect,
            14);
        VERIFY_IS_TRUE(reconnect.Status == RuntimeTerminalLifecycleStatus::WakeRejected);
        VERIFY_IS_TRUE(lifecycle.Snapshot().State == TaskTerminalState::Closed);
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, lifecycle.Snapshot().Generation);

        const auto explicitOpen = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::ExplicitOpen,
            15);
        VERIFY_IS_TRUE(
            explicitOpen.Status == RuntimeTerminalLifecycleStatus::WakeStarted);
        VERIFY_ARE_EQUAL(uint64_t{ 6 }, explicitOpen.Generation);
    }

    void RuntimeTerminalLifecycleTests::WakeFailureIsRecordedAndCanStartAnotherGeneration()
    {
        RuntimeTerminalLifecycleState lifecycle{
            { TaskTerminalState::Suspended, 9, 100 }
        };
        const auto wake = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::NewWork,
            10);
        VERIFY_ARE_EQUAL(uint64_t{ 10 }, wake.Generation);
        const auto stale = lifecycle.CompleteWake(9, true, 11);
        VERIFY_IS_TRUE(stale.Status == RuntimeTerminalLifecycleStatus::StaleGeneration);
        VERIFY_IS_TRUE(lifecycle.Snapshot().State == TaskTerminalState::Starting);

        const auto failed = lifecycle.CompleteWake(10, false, 12);
        VERIFY_IS_TRUE(failed.Status == RuntimeTerminalLifecycleStatus::WakeFailed);
        VERIFY_IS_TRUE(failed.State == TaskTerminalState::Error);
        const auto replay = lifecycle.CompleteWake(10, false, 12);
        VERIFY_IS_TRUE(replay.Status == RuntimeTerminalLifecycleStatus::WakeReplayed);

        const auto retried = lifecycle.RequestWake(
            RuntimeTerminalWakeTrigger::ExplicitOpen,
            13);
        VERIFY_IS_TRUE(retried.Status == RuntimeTerminalLifecycleStatus::WakeStarted);
        VERIFY_ARE_EQUAL(uint64_t{ 11 }, retried.Generation);
    }

    void RuntimeTerminalLifecycleTests::SynchronizationRejectsStaleGenerationAndRearmsReady()
    {
        auto lifecycle = _ready();
        const auto busy = lifecycle.Synchronize(
            TaskTerminalState::Busy,
            7,
            10);
        VERIFY_IS_TRUE(busy.Status == RuntimeTerminalLifecycleStatus::Synchronized);
        VERIFY_IS_TRUE(busy.State == TaskTerminalState::Busy);
        VERIFY_IS_FALSE(busy.Deadline.has_value());

        const auto ready = lifecycle.Synchronize(
            TaskTerminalState::Ready,
            7,
            20);
        VERIFY_IS_TRUE(ready.Status == RuntimeTerminalLifecycleStatus::Synchronized);
        VERIFY_IS_TRUE(ready.Action == RuntimeTerminalLifecycleAction::Wait);
        VERIFY_IS_TRUE(ready.Deadline == std::optional<uint64_t>{ 120 });

        const auto replay = lifecycle.Synchronize(
            TaskTerminalState::Ready,
            7,
            20);
        VERIFY_IS_TRUE(
            replay.Status ==
            RuntimeTerminalLifecycleStatus::SynchronizationReplayed);
        VERIFY_IS_TRUE(replay.Action == RuntimeTerminalLifecycleAction::Wait);
        VERIFY_IS_TRUE(replay.Deadline == ready.Deadline);

        const auto stale = lifecycle.Synchronize(
            TaskTerminalState::Starting,
            6,
            21);
        VERIFY_IS_TRUE(stale.Status == RuntimeTerminalLifecycleStatus::StaleGeneration);
        const auto skipped = lifecycle.Synchronize(
            TaskTerminalState::Starting,
            9,
            22);
        VERIFY_IS_TRUE(skipped.Status == RuntimeTerminalLifecycleStatus::StaleGeneration);
        VERIFY_IS_TRUE(
            lifecycle.Observe(false, 1, 23).Status ==
            RuntimeTerminalLifecycleStatus::Live);
        const auto attachedSuspend = lifecycle.Synchronize(
            TaskTerminalState::Suspended,
            7,
            24);
        VERIFY_IS_TRUE(
            attachedSuspend.Status == RuntimeTerminalLifecycleStatus::InvalidTransition);
        VERIFY_IS_TRUE(lifecycle.Snapshot().State == TaskTerminalState::Ready);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, lifecycle.Snapshot().Generation);
    }

    void RuntimeTerminalLifecycleTests::ClockAndGenerationExhaustionFailClosed()
    {
        auto lifecycle = _ready();
        VERIFY_IS_TRUE(lifecycle.Observe(false, 0, 100).Deadline.has_value());
        const auto regression = lifecycle.Observe(false, 0, 99);
        VERIFY_IS_TRUE(
            regression.Status == RuntimeTerminalLifecycleStatus::ClockRegression);
        VERIFY_IS_FALSE(lifecycle.Valid());
        VERIFY_IS_TRUE(
            lifecycle.Snapshot().Phase == RuntimeTerminalLifecyclePhase::Failed);

        RuntimeTerminalLifecycleState clockExhausted{
            { TaskTerminalState::Ready, 1, 100 }
        };
        const auto maximum = (std::numeric_limits<uint64_t>::max)();
        VERIFY_IS_TRUE(
            clockExhausted.Observe(false, 0, maximum - 99).Status ==
            RuntimeTerminalLifecycleStatus::ClockExhausted);
        VERIFY_IS_FALSE(clockExhausted.Valid());

        RuntimeTerminalLifecycleState generationExhausted{
            { TaskTerminalState::Suspended, maximum, 100 }
        };
        const auto wake = generationExhausted.RequestWake(
            RuntimeTerminalWakeTrigger::ExplicitOpen,
            1);
        VERIFY_IS_TRUE(
            wake.Status == RuntimeTerminalLifecycleStatus::GenerationExhausted);
        VERIFY_IS_TRUE(wake.State == TaskTerminalState::Suspended);
        VERIFY_ARE_EQUAL(maximum, wake.Generation);
    }

    void RuntimeTerminalLifecycleTests::InvalidInitialStateGenerationAndDurationAreRejected()
    {
        RuntimeTerminalLifecycleState zeroDuration{
            { TaskTerminalState::Ready, 1, 0 }
        };
        VERIFY_IS_FALSE(zeroDuration.Valid());
        RuntimeTerminalLifecycleState liveGenerationZero{
            { TaskTerminalState::Ready, 0, 100 }
        };
        VERIFY_IS_FALSE(liveGenerationZero.Valid());
        RuntimeTerminalLifecycleState notCreatedGeneration{
            { TaskTerminalState::NotCreated, 1, 100 }
        };
        VERIFY_IS_FALSE(notCreatedGeneration.Valid());
        RuntimeTerminalLifecycleState invalidState{
            { static_cast<TaskTerminalState>(255), 1, 100 }
        };
        VERIFY_IS_FALSE(invalidState.Valid());

        RuntimeTerminalLifecycleState closedBeforeFirstOpen{
            { TaskTerminalState::Closed, 0, 100 }
        };
        VERIFY_IS_TRUE(closedBeforeFirstOpen.Valid());
        const auto opened = closedBeforeFirstOpen.RequestWake(
            RuntimeTerminalWakeTrigger::ExplicitOpen,
            1);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, opened.Generation);
    }
}
