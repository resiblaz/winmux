// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeClientEpoch.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeClientLocalTests
{
    class RuntimeClientEpochTests
    {
        TEST_CLASS(RuntimeClientEpochTests);

        TEST_METHOD(ActivatesMonotonicEpochsWithoutReuse);
        TEST_METHOD(RejectsBusyAndStaleTransitions);
        TEST_METHOD(RetirementIsFirstWriterWins);
        TEST_METHOD(EpochOverflowFailsClosed);
    };

    void RuntimeClientEpochTests::ActivatesMonotonicEpochsWithoutReuse()
    {
        RuntimeClientEpochState state;
        auto activated = state.Activate();
        VERIFY_IS_TRUE(activated.Status == RuntimeClientEpochActivationStatus::Activated);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, activated.Epoch);
        VERIFY_IS_TRUE(state.IsActive(1));
        VERIFY_IS_TRUE(state.BeginRetire(1, RuntimeClientRetireCause::TransportFailure).Status ==
                       RuntimeClientEpochRetireStatus::Started);
        VERIFY_IS_TRUE(state.CompleteRetire(1));
        VERIFY_IS_TRUE(state.Phase() == RuntimeClientEpochPhase::Offline);

        activated = state.Activate();
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, activated.Epoch);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, state.LastIssued());
    }

    void RuntimeClientEpochTests::RejectsBusyAndStaleTransitions()
    {
        RuntimeClientEpochState state;
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, state.Activate().Epoch);
        const auto busy = state.Activate();
        VERIFY_IS_TRUE(busy.Status == RuntimeClientEpochActivationStatus::Busy);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, busy.Epoch);
        VERIFY_IS_TRUE(state.BeginRetire(2, RuntimeClientRetireCause::ProtocolError).Status ==
                       RuntimeClientEpochRetireStatus::StaleEpoch);
        VERIFY_IS_FALSE(state.CompleteRetire(1));
        VERIFY_IS_TRUE(state.IsActive(1));
    }

    void RuntimeClientEpochTests::RetirementIsFirstWriterWins()
    {
        RuntimeClientEpochState state;
        const auto epoch = state.Activate().Epoch;
        const auto first = state.BeginRetire(epoch, RuntimeClientRetireCause::HeartbeatTimeout);
        const auto second = state.BeginRetire(epoch, RuntimeClientRetireCause::ProtocolError);
        VERIFY_IS_TRUE(first.Status == RuntimeClientEpochRetireStatus::Started);
        VERIFY_IS_TRUE(second.Status == RuntimeClientEpochRetireStatus::AlreadyRetiring);
        VERIFY_IS_TRUE(second.Cause == RuntimeClientRetireCause::HeartbeatTimeout);
        VERIFY_IS_TRUE(state.RetireCause() == RuntimeClientRetireCause::HeartbeatTimeout);
        VERIFY_IS_TRUE(state.CompleteRetire(epoch));
        VERIFY_IS_TRUE(state.BeginRetire(epoch, RuntimeClientRetireCause::Replaced).Status ==
                       RuntimeClientEpochRetireStatus::NoActiveEpoch);

        RuntimeClientEpochState invalidCause;
        const auto invalidEpoch = invalidCause.Activate().Epoch;
        const auto sanitized = invalidCause.BeginRetire(
            invalidEpoch,
            static_cast<RuntimeClientRetireCause>(255));
        VERIFY_IS_TRUE(sanitized.Cause == RuntimeClientRetireCause::ProtocolError);
    }

    void RuntimeClientEpochTests::EpochOverflowFailsClosed()
    {
        constexpr auto maximum = std::numeric_limits<RuntimeClientEpoch>::max();
        RuntimeClientEpochState state{ maximum - 1 };
        const auto last = state.Activate();
        VERIFY_ARE_EQUAL(maximum, last.Epoch);
        const auto busy = state.Activate();
        VERIFY_IS_TRUE(busy.Status == RuntimeClientEpochActivationStatus::Busy);
        VERIFY_ARE_EQUAL(maximum, busy.Epoch);
        VERIFY_IS_TRUE(state.IsActive(maximum));
        VERIFY_IS_TRUE(state.BeginRetire(last.Epoch, RuntimeClientRetireCause::Replaced).Status ==
                       RuntimeClientEpochRetireStatus::Started);
        VERIFY_IS_TRUE(state.CompleteRetire(last.Epoch));
        const auto exhausted = state.Activate();
        VERIFY_IS_TRUE(exhausted.Status == RuntimeClientEpochActivationStatus::Exhausted);
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, exhausted.Epoch);
        VERIFY_IS_TRUE(state.Phase() == RuntimeClientEpochPhase::Exhausted);
    }
}
