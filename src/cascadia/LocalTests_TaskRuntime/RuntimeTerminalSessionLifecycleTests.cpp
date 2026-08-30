// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeTerminalSessionTestSupport.h"

#include <limits>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::RuntimeTerminalSessionTestSupport;

namespace TaskRuntimeLocalTests
{
    class RuntimeTerminalSessionLifecycleTests
    {
        TEST_CLASS(RuntimeTerminalSessionLifecycleTests);

        TEST_METHOD(RejectsInvalidConfigurationAndMissingBackend);
        TEST_METHOD(AttachSnapshotsReplaysAndFansOutToObservers);
        TEST_METHOD(EvictionQueuesResetAndAcknowledgementsRemainMonotonic);
        TEST_METHOD(SlowControllerDisconnectIsTargetedAndReleasesLease);
        TEST_METHOD(LeaseOwnershipDisconnectExpiryAndAbaAreVersioned);
    };

    void RuntimeTerminalSessionLifecycleTests::RejectsInvalidConfigurationAndMissingBackend()
    {
        RuntimeTerminalSession missingBackend{ Configuration(), nullptr };
        VERIFY_IS_FALSE(missingBackend.Valid());
        VERIFY_IS_TRUE(
            missingBackend.Stop().Status ==
            RuntimeTerminalSessionStatus::InvalidConfiguration);

        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        auto invalidGeneration = Configuration();
        invalidGeneration.Generation = 0;
        RuntimeTerminalSession invalidSession{
            invalidGeneration,
            backend,
        };
        VERIFY_IS_FALSE(invalidSession.Valid());
        VERIFY_IS_TRUE(
            invalidSession.Attach(ClientA, Generation, 0, 1).Status ==
            RuntimeTerminalSessionStatus::InvalidConfiguration);

        auto exhaustedSequence = Configuration();
        exhaustedSequence.InitialServerSequence =
            (std::numeric_limits<uint64_t>::max)();
        RuntimeTerminalSession invalidSequence{
            exhaustedSequence,
            backend,
        };
        VERIFY_IS_FALSE(invalidSequence.Valid());

        RuntimeTerminalSession valid{ Configuration(), backend };
        VERIFY_IS_TRUE(valid.Valid());
    }

    void RuntimeTerminalSessionLifecycleTests::AttachSnapshotsReplaysAndFansOutToObservers()
    {
        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        RuntimeTerminalSession session{ Configuration(), backend };

        const auto attachedA = session.Attach(ClientA, Generation, 0, 10);
        VERIFY_IS_TRUE(
            attachedA.Status == RuntimeTerminalSessionStatus::ResetQueued);
        VERIFY_ARE_EQUAL(1u, backend->SnapshotCalls);
        VERIFY_ARE_EQUAL(size_t{ 1 }, session.ClientCount());

        const auto snapshot = session.Pull(ClientA);
        VERIFY_IS_TRUE(snapshot.Status == RuntimeTerminalSessionStatus::FramePulled);
        VERIFY_IS_TRUE(snapshot.Frame.has_value());
        if (!snapshot.Frame)
        {
            return;
        }
        VERIFY_IS_TRUE(snapshot.Frame->Kind == TerminalFrameKind::Output);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, snapshot.Frame->Sequence);
        VERIFY_IS_TRUE(
            (snapshot.Frame->Flags &
             static_cast<uint32_t>(TerminalFrameFlag::ResetBeforePayload)) != 0);
        VERIFY_IS_TRUE(snapshot.Frame->Payload == backend->SnapshotPayload);

        const auto published = session.PublishOutput({ 'o', 'n', 'e' }, 0, 11);
        VERIFY_IS_TRUE(
            published.Status == RuntimeTerminalSessionStatus::OutputPublished);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, session.LatestOutputSequence());

        const auto stale = session.Attach(ClientB, Generation + 1, 2, 12);
        VERIFY_IS_TRUE(stale.Status == RuntimeTerminalSessionStatus::StaleGeneration);
        VERIFY_ARE_EQUAL(size_t{ 1 }, session.ClientCount());
        const auto ahead = session.Attach(ClientB, Generation, 4, 12);
        VERIFY_IS_TRUE(
            ahead.Status == RuntimeTerminalSessionStatus::AcknowledgementAhead);
        VERIFY_ARE_EQUAL(size_t{ 1 }, session.ClientCount());

        const auto replayed = session.Attach(ClientB, Generation, 2, 12);
        VERIFY_IS_TRUE(
            replayed.Status == RuntimeTerminalSessionStatus::ReplayQueued);
        const auto liveForA = session.Pull(ClientA);
        const auto replayForB = session.Pull(ClientB);
        VERIFY_IS_TRUE(liveForA.Frame.has_value());
        VERIFY_IS_TRUE(replayForB.Frame.has_value());
        if (!liveForA.Frame || !replayForB.Frame)
        {
            return;
        }
        VERIFY_ARE_EQUAL(liveForA.Frame->Sequence, replayForB.Frame->Sequence);
        VERIFY_IS_TRUE(liveForA.Frame->Payload == replayForB.Frame->Payload);

        const auto upToDate = session.Attach(ClientC, Generation, 3, 13);
        VERIFY_IS_TRUE(upToDate.Status == RuntimeTerminalSessionStatus::Attached);
        VERIFY_IS_TRUE(session.Pull(ClientC).Status == RuntimeTerminalSessionStatus::Empty);
        VERIFY_IS_TRUE(
            session.Attach(ClientA, Generation, 3, 13).Status ==
            RuntimeTerminalSessionStatus::AlreadyAttached);
        VERIFY_ARE_EQUAL(1u, backend->SnapshotCalls);
        VERIFY_ARE_EQUAL(size_t{ 3 }, session.ClientCount());
    }

    void RuntimeTerminalSessionLifecycleTests::EvictionQueuesResetAndAcknowledgementsRemainMonotonic()
    {
        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        auto configuration = Configuration();
        configuration.MaximumOutputRingFrames = 2;
        RuntimeTerminalSession session{ configuration, backend };

        VERIFY_IS_TRUE(
            session.Attach(ClientA, Generation, 0, 10).Status ==
            RuntimeTerminalSessionStatus::ResetQueued);
        (void)session.Pull(ClientA);
        VERIFY_IS_TRUE(
            session.PublishOutput({ 'a' }, 0, 11).Status ==
            RuntimeTerminalSessionStatus::OutputPublished);
        VERIFY_IS_TRUE(
            session.PublishOutput({ 'b' }, 0, 12).Status ==
            RuntimeTerminalSessionStatus::OutputPublished);
        VERIFY_ARE_EQUAL(uint64_t{ 4 }, session.LatestOutputSequence());
        VERIFY_IS_TRUE(
            session.Detach(ClientA, 13).Status ==
            RuntimeTerminalSessionStatus::Detached);

        const auto reset = session.Attach(ClientB, Generation, 0, 14);
        VERIFY_IS_TRUE(reset.Status == RuntimeTerminalSessionStatus::ResetQueued);
        VERIFY_ARE_EQUAL(2u, backend->SnapshotCalls);
        const auto resetFrame = session.Pull(ClientB);
        VERIFY_IS_TRUE(resetFrame.Frame.has_value());
        if (!resetFrame.Frame)
        {
            return;
        }
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, resetFrame.Frame->Sequence);
        VERIFY_IS_TRUE(
            (resetFrame.Frame->Flags &
             static_cast<uint32_t>(TerminalFrameFlag::ResetBeforePayload)) != 0);

        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientB,
                Acknowledge(20, 5),
                15,
                std::nullopt).Status == RuntimeTerminalSessionStatus::Acknowledged);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientB,
                Acknowledge(21, 5),
                15,
                std::nullopt).Status ==
            RuntimeTerminalSessionStatus::DuplicateAcknowledgement);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientB,
                Acknowledge(22, 4),
                15,
                std::nullopt).Status ==
            RuntimeTerminalSessionStatus::AcknowledgementRegression);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientB,
                Acknowledge(23, 6),
                15,
                std::nullopt).Status ==
            RuntimeTerminalSessionStatus::AcknowledgementAhead);
    }

    void RuntimeTerminalSessionLifecycleTests::SlowControllerDisconnectIsTargetedAndReleasesLease()
    {
        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        auto configuration = Configuration();
        configuration.MaximumQueuedFramesPerClient = 2;
        RuntimeTerminalSession session{ configuration, backend };

        (void)session.Attach(ClientA, Generation, 0, 10);
        VERIFY_IS_TRUE(
            session.Attach(ClientB, Generation, 2, 10).Status ==
            RuntimeTerminalSessionStatus::Attached);
        const auto lease = session.AcquireInputLease(ClientA, 10);
        VERIFY_IS_TRUE(lease.Status == RuntimeTerminalSessionStatus::LeaseAcquired);
        VERIFY_IS_TRUE(lease.Lease.has_value());

        VERIFY_IS_TRUE(
            session.PublishOutput({ 'x' }, 0, 11).Status ==
            RuntimeTerminalSessionStatus::OutputPublished);
        const auto firstForObserver = session.Pull(ClientB);
        VERIFY_IS_TRUE(firstForObserver.Frame.has_value());
        if (!firstForObserver.Frame)
        {
            return;
        }
        VERIFY_IS_TRUE(firstForObserver.Frame->Payload == std::vector<uint8_t>{ 'x' });

        const auto second = session.PublishOutput({ 'y' }, 0, 12);
        VERIFY_IS_TRUE(
            second.Status ==
            RuntimeTerminalSessionStatus::OutputPublishedWithSlowPeerDisconnect);
        VERIFY_IS_TRUE((
            second.DisconnectedClients ==
            std::vector<std::string>{ std::string{ ClientA } }));
        VERIFY_ARE_EQUAL(size_t{ 1 }, session.ClientCount());
        VERIFY_IS_FALSE(session.CurrentLease().has_value());
        VERIFY_IS_TRUE(
            session.Pull(ClientA).Status == RuntimeTerminalSessionStatus::NotAttached);

        const auto secondForObserver = session.Pull(ClientB);
        VERIFY_IS_TRUE(secondForObserver.Frame.has_value());
        if (!secondForObserver.Frame)
        {
            return;
        }
        VERIFY_IS_TRUE(secondForObserver.Frame->Payload == std::vector<uint8_t>{ 'y' });
        const auto observerLease = session.AcquireInputLease(ClientB, 13);
        VERIFY_IS_TRUE(
            observerLease.Status == RuntimeTerminalSessionStatus::LeaseAcquired);
        VERIFY_IS_TRUE(observerLease.Lease.has_value());
    }

    void RuntimeTerminalSessionLifecycleTests::LeaseOwnershipDisconnectExpiryAndAbaAreVersioned()
    {
        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        RuntimeTerminalSession session{ Configuration(), backend };
        (void)session.Attach(ClientA, Generation, 0, 1);
        (void)session.Pull(ClientA);
        (void)session.Attach(ClientB, Generation, 2, 1);

        const auto first = session.AcquireInputLease(ClientA, 10);
        VERIFY_IS_TRUE(first.Lease.has_value());
        if (!first.Lease)
        {
            return;
        }
        const auto firstRevision = first.Lease->LeaseRevision;
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(29, { 'x' }),
                9,
                firstRevision).Status ==
            RuntimeTerminalSessionStatus::LeaseClockRegression);
        VERIFY_IS_TRUE(
            session.AcquireInputLease(ClientB, 11).Status ==
            RuntimeTerminalSessionStatus::LeaseBusy);
        VERIFY_IS_TRUE(
            session.ReleaseInputLease(ClientA, firstRevision + 1, 12).Status ==
            RuntimeTerminalSessionStatus::StaleLease);
        VERIFY_IS_TRUE(session.CurrentLease().has_value());
        VERIFY_IS_TRUE(
            session.ReleaseInputLease(ClientA, firstRevision, 13).Status ==
            RuntimeTerminalSessionStatus::LeaseReleased);

        const auto second = session.AcquireInputLease(ClientB, 14);
        VERIFY_IS_TRUE(second.Lease.has_value());
        if (!second.Lease)
        {
            return;
        }
        VERIFY_ARE_EQUAL(firstRevision + 1, second.Lease->LeaseRevision);
        VERIFY_IS_TRUE(
            session.Detach(ClientB, 15).Status ==
            RuntimeTerminalSessionStatus::Detached);
        VERIFY_IS_FALSE(session.CurrentLease().has_value());

        const auto third = session.AcquireInputLease(ClientA, 20);
        VERIFY_IS_TRUE(third.Lease.has_value());
        if (!third.Lease)
        {
            return;
        }
        const auto expiredRevision = third.Lease->LeaseRevision;
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(30, { 'x' }),
                120,
                expiredRevision).Status == RuntimeTerminalSessionStatus::LeaseExpired);
        VERIFY_IS_FALSE(session.CurrentLease().has_value());
        VERIFY_ARE_EQUAL(0u, backend->WriteInputCalls);

        const auto fourth = session.AcquireInputLease(ClientA, 120);
        VERIFY_IS_TRUE(fourth.Lease.has_value());
        if (!fourth.Lease)
        {
            return;
        }
        const auto currentRevision = fourth.Lease->LeaseRevision;
        VERIFY_ARE_EQUAL(expiredRevision + 1, currentRevision);
        VERIFY_IS_TRUE(
            session.ReleaseInputLease(ClientA, expiredRevision, 121).Status ==
            RuntimeTerminalSessionStatus::StaleLease);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(31, { 'y' }),
                122,
                expiredRevision).Status == RuntimeTerminalSessionStatus::StaleLease);
        VERIFY_ARE_EQUAL(0u, backend->WriteInputCalls);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(32, { 'z' }),
                123,
                currentRevision).Status == RuntimeTerminalSessionStatus::InputAccepted);
        VERIFY_ARE_EQUAL(1u, backend->WriteInputCalls);
        VERIFY_IS_TRUE(backend->Inputs.front() == std::vector<uint8_t>{ 'z' });
    }
}
