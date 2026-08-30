// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeHelperIdleState.h"

#include <limits>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        RuntimeHelperIdleState _state(const uint32_t grace = 100)
        {
            return RuntimeHelperIdleState{ { grace } };
        }
    }

    class RuntimeHelperIdleStateTests
    {
        TEST_CLASS(RuntimeHelperIdleStateTests);

        TEST_METHOD(DefaultGraceIsTwoMinutesAndExactExpiryExits);
        TEST_METHOD(EveryActivityClassCancelsAndRestartsGrace);
        TEST_METHOD(CompletedConnectionSequenceRestartsGrace);
        TEST_METHOD(ActivityWinsAtTheExactDeadline);
        TEST_METHOD(UnavailableObservationFailsIdleClosed);
        TEST_METHOD(NewActivityCanCancelAnExitReadyDecision);
        TEST_METHOD(ClockAndActivitySequenceRegressionsFailClosed);
        TEST_METHOD(InvalidDurationAndClockExhaustionAreRejected);
    };

    void RuntimeHelperIdleStateTests::DefaultGraceIsTwoMinutesAndExactExpiryExits()
    {
        VERIFY_ARE_EQUAL(uint32_t{ 120000 }, DefaultRuntimeHelperIdleExitMilliseconds);
        RuntimeHelperIdleState state;
        VERIFY_IS_TRUE(state.Valid());
        const auto armed = state.Observe({}, 1000);
        VERIFY_IS_TRUE(armed.Status == RuntimeHelperIdleStatus::IdleGraceStarted);
        VERIFY_IS_TRUE(armed.Action == RuntimeHelperIdleAction::Wait);
        VERIFY_IS_TRUE(armed.Deadline == std::optional<uint64_t>{ 121000 });
        VERIFY_IS_TRUE(
            state.Observe({}, 120999).Status ==
            RuntimeHelperIdleStatus::IdleGraceContinues);
        const auto exit = state.Observe({}, 121000);
        VERIFY_IS_TRUE(exit.Status == RuntimeHelperIdleStatus::ExitReady);
        VERIFY_IS_TRUE(exit.Action == RuntimeHelperIdleAction::ExitRuntime);
    }

    void RuntimeHelperIdleStateTests::EveryActivityClassCancelsAndRestartsGrace()
    {
        const std::array activities{
            RuntimeHelperActivitySnapshot{ .ControlClients = 1 },
            RuntimeHelperActivitySnapshot{ .TerminalClients = 1 },
            RuntimeHelperActivitySnapshot{ .ActiveRuns = 1 },
            RuntimeHelperActivitySnapshot{ .PendingWork = 1 },
            RuntimeHelperActivitySnapshot{ .LiveTerminalSessions = 1 },
        };
        for (const auto& activity : activities)
        {
            auto state = _state();
            VERIFY_IS_TRUE(state.Observe({}, 10).Deadline == std::optional<uint64_t>{ 110 });
            const auto active = state.Observe(activity, 110);
            VERIFY_IS_TRUE(active.Status == RuntimeHelperIdleStatus::Active);
            VERIFY_IS_FALSE(active.Deadline.has_value());
            const auto restarted = state.Observe({}, 120);
            VERIFY_IS_TRUE(restarted.Deadline == std::optional<uint64_t>{ 220 });
        }
    }

    void RuntimeHelperIdleStateTests::CompletedConnectionSequenceRestartsGrace()
    {
        auto state = _state();
        RuntimeHelperActivitySnapshot activity;
        activity.ActivitySequence = 4;
        VERIFY_IS_TRUE(
            state.Observe(activity, 10).Deadline ==
            std::optional<uint64_t>{ 110 });
        activity.ActivitySequence = 5;
        const auto restarted = state.Observe(activity, 100);
        VERIFY_IS_TRUE(restarted.Status == RuntimeHelperIdleStatus::IdleGraceStarted);
        VERIFY_IS_TRUE(restarted.Deadline == std::optional<uint64_t>{ 200 });
        VERIFY_IS_TRUE(
            state.Observe(activity, 110).Status ==
            RuntimeHelperIdleStatus::IdleGraceContinues);
    }

    void RuntimeHelperIdleStateTests::ActivityWinsAtTheExactDeadline()
    {
        auto state = _state();
        (void)state.Observe({}, 10);
        RuntimeHelperActivitySnapshot activity;
        activity.ActiveRuns = 1;
        const auto active = state.Observe(activity, 110);
        VERIFY_IS_TRUE(active.Status == RuntimeHelperIdleStatus::Active);
        VERIFY_IS_TRUE(active.Action == RuntimeHelperIdleAction::None);
        VERIFY_IS_FALSE(active.Deadline.has_value());
    }

    void RuntimeHelperIdleStateTests::UnavailableObservationFailsIdleClosed()
    {
        auto state = _state();
        (void)state.Observe({}, 10);
        RuntimeHelperActivitySnapshot unavailable;
        unavailable.Observed = false;
        const auto blocked = state.Observe(unavailable, 110);
        VERIFY_IS_TRUE(
            blocked.Status == RuntimeHelperIdleStatus::ObservationUnavailable);
        VERIFY_IS_FALSE(blocked.Deadline.has_value());
        VERIFY_IS_TRUE(state.Valid());

        const auto recovered = state.Observe({}, 120);
        VERIFY_IS_TRUE(recovered.Deadline == std::optional<uint64_t>{ 220 });
    }

    void RuntimeHelperIdleStateTests::NewActivityCanCancelAnExitReadyDecision()
    {
        auto state = _state();
        (void)state.Observe({}, 10);
        VERIFY_IS_TRUE(
            state.Observe({}, 110).Action == RuntimeHelperIdleAction::ExitRuntime);
        RuntimeHelperActivitySnapshot activity;
        activity.ActivitySequence = 1;
        activity.ControlClients = 1;
        const auto cancelled = state.Observe(activity, 110);
        VERIFY_IS_TRUE(cancelled.Status == RuntimeHelperIdleStatus::Active);
        VERIFY_IS_TRUE(state.Snapshot().Phase == RuntimeHelperIdlePhase::Active);
    }

    void RuntimeHelperIdleStateTests::ClockAndActivitySequenceRegressionsFailClosed()
    {
        auto clock = _state();
        (void)clock.Observe({}, 10);
        VERIFY_IS_TRUE(
            clock.Observe({}, 9).Status == RuntimeHelperIdleStatus::ClockRegression);
        VERIFY_IS_FALSE(clock.Valid());

        auto sequence = _state();
        RuntimeHelperActivitySnapshot activity;
        activity.ActivitySequence = 2;
        (void)sequence.Observe(activity, 10);
        activity.ActivitySequence = 1;
        VERIFY_IS_TRUE(
            sequence.Observe(activity, 11).Status ==
            RuntimeHelperIdleStatus::ActivitySequenceRegression);
        VERIFY_IS_FALSE(sequence.Valid());
    }

    void RuntimeHelperIdleStateTests::InvalidDurationAndClockExhaustionAreRejected()
    {
        RuntimeHelperIdleState invalid{ { 0 } };
        VERIFY_IS_FALSE(invalid.Valid());
        VERIFY_IS_TRUE(
            invalid.Observe({}, 0).Status ==
            RuntimeHelperIdleStatus::InvalidConfiguration);

        auto exhausted = _state();
        const auto maximum = (std::numeric_limits<uint64_t>::max)();
        VERIFY_IS_TRUE(
            exhausted.Observe({}, maximum - 99).Status ==
            RuntimeHelperIdleStatus::ClockExhausted);
        VERIFY_IS_FALSE(exhausted.Valid());
    }
}
