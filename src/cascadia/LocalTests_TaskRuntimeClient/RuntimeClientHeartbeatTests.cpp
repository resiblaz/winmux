// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeClientHeartbeat.h"

#include "ProtocolValidation.h"

#include <array>
#include <limits>
#include <unordered_set>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        constexpr RuntimeClientEpoch EpochOne{ 7 };
        constexpr RuntimeClientEpoch EpochTwo{ 8 };
        constexpr RuntimeClientOperationId PingOne{ 41 };
        constexpr RuntimeClientOperationId PingTwo{ 42 };
        constexpr std::string_view ClientOne{ "10000000-0000-4000-8000-000000000401" };
        constexpr std::string_view ClientTwo{ "10000000-0000-4000-8000-000000000402" };

        RuntimeClientHeartbeatView View(const RuntimeClientHeartbeatState& state)
        {
            const auto view = state.ActiveView();
            VERIFY_IS_TRUE(view.has_value());
            return view.value_or(RuntimeClientHeartbeatView{});
        }

        void MakePingInFlight(
            RuntimeClientHeartbeatState& state,
            const RuntimeClientOperationId operationId = PingOne)
        {
            VERIFY_IS_TRUE(state.Initialize(EpochOne, 0, 1000));
            VERIFY_IS_TRUE(state.Decide(500).Action == RuntimeClientHeartbeatAction::QueuePing);
            VERIFY_IS_TRUE(state.BeginPing(operationId));
        }
    }

    class RuntimeClientHeartbeatTests
    {
        TEST_CLASS(RuntimeClientHeartbeatTests);

        TEST_METHOD(InitialWindowUsesHalfAndFullDeadlines);
        TEST_METHOD(OddIntervalUsesFloorHalfWithForwardProgress);
        TEST_METHOD(FullDeadlineWinsOverPingDue);
        TEST_METHOD(PingDueQueuesOnceAndInflightWaitsForFullDeadline);
        TEST_METHOD(InboundSupersedesPingAndRearmsWindow);
        TEST_METHOD(InboundAndAckAtFullDeadlineDoNotRearm);
        TEST_METHOD(AcknowledgeRequiresExactOperationAndRejectsOldAck);
        TEST_METHOD(RetireRejectsOldEpochOperationAfterReinitialize);
        TEST_METHOD(BackwardClockFailsClosed);
        TEST_METHOD(InitializationAndObservationOverflowFailClosed);
        TEST_METHOD(HeartbeatRequestIdsAreCanonicalReservedAndUnique);
    };

    void RuntimeClientHeartbeatTests::InitialWindowUsesHalfAndFullDeadlines()
    {
        RuntimeClientHeartbeatState state;
        VERIFY_IS_TRUE(state.Initialize(EpochOne, 0, 1000));
        const auto initial = View(state);
        VERIFY_IS_TRUE(initial.Phase == RuntimeClientHeartbeatPhase::Monitoring);
        VERIFY_ARE_EQUAL(EpochOne, initial.Epoch);
        VERIFY_ARE_EQUAL(uint32_t{ 1000 }, initial.IntervalMilliseconds);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 0 }, initial.LastInbound);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 500 }, initial.PingDue);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 1000 }, initial.SilenceDeadline);
        VERIFY_IS_FALSE(initial.HasWriterWork);

        const auto before = state.Decide(499);
        VERIFY_IS_TRUE(before.Action == RuntimeClientHeartbeatAction::Wait);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 500 }, before.Deadline);
        const auto due = state.Decide(500);
        VERIFY_IS_TRUE(due.Action == RuntimeClientHeartbeatAction::QueuePing);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 1000 }, due.Deadline);
    }

    void RuntimeClientHeartbeatTests::OddIntervalUsesFloorHalfWithForwardProgress()
    {
        RuntimeClientHeartbeatState odd;
        VERIFY_IS_TRUE(odd.Initialize(EpochOne, 10, 251));
        const auto oddView = View(odd);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 135 }, oddView.PingDue);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 261 }, oddView.SilenceDeadline);
        VERIFY_IS_TRUE(oddView.PingDue > oddView.LastInbound);
        VERIFY_IS_TRUE(odd.Decide(134).Action == RuntimeClientHeartbeatAction::Wait);
        VERIFY_IS_TRUE(odd.Decide(135).Action == RuntimeClientHeartbeatAction::QueuePing);

        RuntimeClientHeartbeatState minimum;
        VERIFY_IS_TRUE(minimum.Initialize(
            EpochOne,
            20,
            MinimumRuntimeClientHeartbeatMilliseconds));
        VERIFY_IS_TRUE(View(minimum).PingDue > View(minimum).LastInbound);
    }

    void RuntimeClientHeartbeatTests::FullDeadlineWinsOverPingDue()
    {
        RuntimeClientHeartbeatState state;
        VERIFY_IS_TRUE(state.Initialize(EpochOne, 0, 1000));
        const auto decision = state.Decide(1000);
        VERIFY_IS_TRUE(decision.Action == RuntimeClientHeartbeatAction::Timeout);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 1000 }, decision.Deadline);
        VERIFY_IS_TRUE(View(state).Phase == RuntimeClientHeartbeatPhase::TimedOut);
        VERIFY_IS_FALSE(state.HasWriterWork());
        VERIFY_IS_TRUE(state.Decide(1500).Action == RuntimeClientHeartbeatAction::Timeout);
    }

    void RuntimeClientHeartbeatTests::PingDueQueuesOnceAndInflightWaitsForFullDeadline()
    {
        RuntimeClientHeartbeatState state;
        VERIFY_IS_TRUE(state.Initialize(EpochOne, 0, 1000));
        VERIFY_IS_TRUE(state.Decide(500).Action == RuntimeClientHeartbeatAction::QueuePing);
        VERIFY_IS_TRUE(state.HasWriterWork());
        const auto repeated = state.Decide(500);
        VERIFY_IS_TRUE(repeated.Action == RuntimeClientHeartbeatAction::Wait);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 1000 }, repeated.Deadline);

        VERIFY_IS_FALSE(state.BeginPing(0));
        VERIFY_IS_TRUE(state.BeginPing(PingOne));
        VERIFY_IS_FALSE(state.BeginPing(PingTwo));
        const auto inflight = View(state);
        VERIFY_IS_TRUE(inflight.Phase == RuntimeClientHeartbeatPhase::PingInFlight);
        VERIFY_ARE_EQUAL(PingOne, inflight.PingOperationId);
        VERIFY_IS_FALSE(state.HasWriterWork());
        const auto waiting = state.Decide(999);
        VERIFY_IS_TRUE(waiting.Action == RuntimeClientHeartbeatAction::Wait);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 1000 }, waiting.Deadline);
        VERIFY_IS_TRUE(state.Decide(1000).Action == RuntimeClientHeartbeatAction::Timeout);
    }

    void RuntimeClientHeartbeatTests::InboundSupersedesPingAndRearmsWindow()
    {
        RuntimeClientHeartbeatState state;
        MakePingInFlight(state);
        const auto inbound = state.ObserveInbound(600);
        VERIFY_IS_TRUE(inbound.Status == RuntimeClientHeartbeatStatus::Succeeded);
        VERIFY_IS_TRUE(inbound.SupersededOperation.has_value());
        VERIFY_ARE_EQUAL(PingOne, inbound.SupersededOperation.value_or(0));

        const auto refreshed = View(state);
        VERIFY_IS_TRUE(refreshed.Phase == RuntimeClientHeartbeatPhase::Monitoring);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 600 }, refreshed.LastInbound);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 1100 }, refreshed.PingDue);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 1600 }, refreshed.SilenceDeadline);
        VERIFY_ARE_EQUAL(RuntimeClientOperationId{ 0 }, refreshed.PingOperationId);
        VERIFY_IS_FALSE(refreshed.HasWriterWork);
        VERIFY_IS_TRUE(state.Decide(1099).Action == RuntimeClientHeartbeatAction::Wait);
        VERIFY_IS_TRUE(state.Decide(1100).Action == RuntimeClientHeartbeatAction::QueuePing);
    }

    void RuntimeClientHeartbeatTests::InboundAndAckAtFullDeadlineDoNotRearm()
    {
        RuntimeClientHeartbeatState inbound;
        MakePingInFlight(inbound);
        const auto rejected = inbound.ObserveInbound(1000);
        VERIFY_IS_TRUE(rejected.Status == RuntimeClientHeartbeatStatus::Failed);
        VERIFY_IS_FALSE(rejected.SupersededOperation.has_value());
        const auto timedOut = View(inbound);
        VERIFY_IS_TRUE(timedOut.Phase == RuntimeClientHeartbeatPhase::TimedOut);
        VERIFY_ARE_EQUAL(PingOne, timedOut.PingOperationId);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 0 }, timedOut.LastInbound);

        RuntimeClientHeartbeatState acknowledgement;
        MakePingInFlight(acknowledgement);
        VERIFY_IS_TRUE(acknowledgement.AcknowledgePing(PingOne, 1001) ==
                       RuntimeClientHeartbeatStatus::Failed);
        const auto ackTimedOut = View(acknowledgement);
        VERIFY_IS_TRUE(ackTimedOut.Phase == RuntimeClientHeartbeatPhase::TimedOut);
        VERIFY_ARE_EQUAL(PingOne, ackTimedOut.PingOperationId);
    }

    void RuntimeClientHeartbeatTests::AcknowledgeRequiresExactOperationAndRejectsOldAck()
    {
        RuntimeClientHeartbeatState state;
        MakePingInFlight(state);
        const auto before = View(state);
        VERIFY_IS_TRUE(state.AcknowledgePing(PingTwo, 600) == RuntimeClientHeartbeatStatus::Stale);
        VERIFY_IS_TRUE(state.AcknowledgePing(0, 600) == RuntimeClientHeartbeatStatus::Stale);
        const auto afterWrong = View(state);
        VERIFY_IS_TRUE(afterWrong.Phase == before.Phase);
        VERIFY_ARE_EQUAL(before.LastInbound, afterWrong.LastInbound);
        VERIFY_ARE_EQUAL(before.PingOperationId, afterWrong.PingOperationId);

        VERIFY_IS_TRUE(state.AcknowledgePing(PingOne, 600) ==
                       RuntimeClientHeartbeatStatus::Succeeded);
        const auto acknowledged = View(state);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 600 }, acknowledged.LastInbound);
        VERIFY_ARE_EQUAL(RuntimeClientMonotonicMilliseconds{ 1100 }, acknowledged.PingDue);
        VERIFY_IS_TRUE(state.AcknowledgePing(PingOne, 900) == RuntimeClientHeartbeatStatus::Stale);
        const auto afterOld = View(state);
        VERIFY_ARE_EQUAL(acknowledged.LastInbound, afterOld.LastInbound);
        VERIFY_ARE_EQUAL(acknowledged.PingDue, afterOld.PingDue);
        VERIFY_ARE_EQUAL(acknowledged.SilenceDeadline, afterOld.SilenceDeadline);
    }

    void RuntimeClientHeartbeatTests::RetireRejectsOldEpochOperationAfterReinitialize()
    {
        RuntimeClientHeartbeatState state;
        MakePingInFlight(state);
        state.Retire();
        VERIFY_IS_FALSE(state.ActiveView().has_value());
        VERIFY_IS_FALSE(state.HasWriterWork());
        VERIFY_IS_TRUE(state.ObserveInbound(100).Status == RuntimeClientHeartbeatStatus::Inactive);
        VERIFY_IS_TRUE(state.AcknowledgePing(PingOne, 100) ==
                       RuntimeClientHeartbeatStatus::Inactive);

        VERIFY_IS_TRUE(state.Initialize(EpochTwo, 100, 1000));
        VERIFY_ARE_EQUAL(EpochTwo, View(state).Epoch);
        VERIFY_IS_TRUE(state.Decide(600).Action == RuntimeClientHeartbeatAction::QueuePing);
        VERIFY_IS_TRUE(state.BeginPing(PingTwo));
        VERIFY_IS_TRUE(state.AcknowledgePing(PingOne, 700) == RuntimeClientHeartbeatStatus::Stale);
        VERIFY_ARE_EQUAL(PingTwo, View(state).PingOperationId);
        VERIFY_IS_TRUE(state.AcknowledgePing(PingTwo, 700) ==
                       RuntimeClientHeartbeatStatus::Succeeded);
    }

    void RuntimeClientHeartbeatTests::BackwardClockFailsClosed()
    {
        RuntimeClientHeartbeatState decision;
        VERIFY_IS_TRUE(decision.Initialize(EpochOne, 100, 1000));
        VERIFY_IS_TRUE(decision.Decide(99).Action == RuntimeClientHeartbeatAction::Failed);
        VERIFY_IS_TRUE(View(decision).Phase == RuntimeClientHeartbeatPhase::Failed);

        RuntimeClientHeartbeatState inbound;
        VERIFY_IS_TRUE(inbound.Initialize(EpochOne, 100, 1000));
        VERIFY_IS_TRUE(inbound.ObserveInbound(99).Status == RuntimeClientHeartbeatStatus::Failed);
        VERIFY_IS_TRUE(View(inbound).Phase == RuntimeClientHeartbeatPhase::Failed);

        RuntimeClientHeartbeatState acknowledgement;
        VERIFY_IS_TRUE(acknowledgement.Initialize(EpochOne, 100, 1000));
        VERIFY_IS_TRUE(acknowledgement.Decide(600).Action == RuntimeClientHeartbeatAction::QueuePing);
        VERIFY_IS_TRUE(acknowledgement.BeginPing(PingOne));
        VERIFY_IS_TRUE(acknowledgement.AcknowledgePing(PingOne, 99) ==
                       RuntimeClientHeartbeatStatus::Failed);
    }

    void RuntimeClientHeartbeatTests::InitializationAndObservationOverflowFailClosed()
    {
        constexpr auto maximum =
            (std::numeric_limits<RuntimeClientMonotonicMilliseconds>::max)();
        RuntimeClientHeartbeatState exact;
        VERIFY_IS_TRUE(exact.Initialize(EpochOne, maximum - 1000, 1000));
        VERIFY_ARE_EQUAL(maximum, View(exact).SilenceDeadline);

        RuntimeClientHeartbeatState overflow;
        VERIFY_IS_FALSE(overflow.Initialize(EpochOne, maximum - 999, 1000));
        VERIFY_IS_TRUE(View(overflow).Phase == RuntimeClientHeartbeatPhase::Failed);

        RuntimeClientHeartbeatState observation;
        VERIFY_IS_TRUE(observation.Initialize(EpochOne, maximum - 1000, 1000));
        VERIFY_IS_TRUE(observation.ObserveInbound(maximum - 999).Status ==
                       RuntimeClientHeartbeatStatus::Failed);
        VERIFY_IS_TRUE(View(observation).Phase == RuntimeClientHeartbeatPhase::Failed);

        RuntimeClientHeartbeatState zeroEpoch;
        VERIFY_IS_FALSE(zeroEpoch.Initialize(0, 0, 1000));
        RuntimeClientHeartbeatState tooShort;
        VERIFY_IS_FALSE(tooShort.Initialize(
            EpochOne,
            0,
            MinimumRuntimeClientHeartbeatMilliseconds - 1));
        RuntimeClientHeartbeatState tooLong;
        VERIFY_IS_FALSE(tooLong.Initialize(
            EpochOne,
            0,
            MaximumRuntimeClientHeartbeatMilliseconds + 1));
    }

    void RuntimeClientHeartbeatTests::HeartbeatRequestIdsAreCanonicalReservedAndUnique()
    {
        const auto namespaceOne = RuntimeClientHeartbeatNamespace(ClientOne);
        const auto namespaceOneAgain = RuntimeClientHeartbeatNamespace(ClientOne);
        const auto namespaceTwo = RuntimeClientHeartbeatNamespace(ClientTwo);
        VERIFY_IS_TRUE(namespaceOne.has_value());
        VERIFY_IS_TRUE(namespaceOne == namespaceOneAgain);
        VERIFY_IS_TRUE(namespaceTwo.has_value());
        VERIFY_IS_TRUE(namespaceOne != namespaceTwo);
        VERIFY_IS_FALSE(RuntimeClientHeartbeatNamespace("not-a-uuid").has_value());

        const auto oneOne = MakeRuntimeClientHeartbeatRequestId(ClientOne, 1);
        const auto oneTwo = MakeRuntimeClientHeartbeatRequestId(ClientOne, 2);
        const auto twoOne = MakeRuntimeClientHeartbeatRequestId(ClientTwo, 1);
        const auto maximum = MakeRuntimeClientHeartbeatRequestId(
            ClientOne,
            (std::numeric_limits<uint64_t>::max)());
        VERIFY_IS_TRUE(oneOne && oneTwo && twoOne && maximum);
        if (!oneOne || !oneTwo || !twoOne || !maximum)
        {
            return;
        }

        const std::array generated{ *oneOne, *oneTwo, *twoOne, *maximum };
        std::unordered_set<std::string> unique;
        for (const auto& requestId : generated)
        {
            VERIFY_IS_TRUE(IsCanonicalProtocolUuid(requestId));
            VERIFY_IS_TRUE(IsReservedHeartbeatRequestId(requestId));
            VERIFY_IS_TRUE(requestId.starts_with(RuntimeClientHeartbeatRequestIdPrefix));
            unique.emplace(requestId);
        }
        VERIFY_ARE_EQUAL(generated.size(), unique.size());
        const auto parsed = ParseRuntimeClientHeartbeatRequestId(*oneOne);
        VERIFY_IS_TRUE(parsed.has_value());
        VERIFY_ARE_EQUAL(namespaceOne.value_or(0), parsed ? parsed->Namespace : 0);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, parsed ? parsed->Sequence : 0);
        const auto parsedMaximum = ParseRuntimeClientHeartbeatRequestId(*maximum);
        VERIFY_IS_TRUE(parsedMaximum.has_value());
        VERIFY_ARE_EQUAL(
            (std::numeric_limits<uint64_t>::max)(),
            parsedMaximum ? parsedMaximum->Sequence : 0);
        VERIFY_IS_FALSE(MakeRuntimeClientHeartbeatRequestId(ClientOne, 0).has_value());
        VERIFY_IS_FALSE(MakeRuntimeClientHeartbeatRequestId("not-a-uuid", 1).has_value());
        VERIFY_IS_FALSE(ParseRuntimeClientHeartbeatRequestId(
                            "f17e0001-0000-0000-0000-000000000000")
                            .has_value());
        VERIFY_IS_TRUE(IsReservedHeartbeatRequestId(
            "f17e0001-0000-0000-0000-000000000000"));
        VERIFY_IS_FALSE(IsReservedHeartbeatRequestId(
            "30000000-0000-4000-8000-000000000401"));
    }
}
