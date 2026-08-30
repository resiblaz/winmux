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
    class RuntimeTerminalSessionFrameTests
    {
        TEST_CLASS(RuntimeTerminalSessionFrameTests);

        TEST_METHOD(OnlyControllerMayWriteInputAndResize);
        TEST_METHOD(EscapeAndControlStopConvergeOnBackendInterrupt);
        TEST_METHOD(SnapshotFailuresDetachOnlyTheAttachingClient);
        TEST_METHOD(BackendOperationFailuresAreContainedAndReported);
        TEST_METHOD(HeartbeatValidationAndSequenceExhaustionFailClosed);
        TEST_METHOD(InvalidOutputDoesNotAdvanceAndEndOfStreamIsPublished);
    };

    void RuntimeTerminalSessionFrameTests::OnlyControllerMayWriteInputAndResize()
    {
        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        RuntimeTerminalSession session{ Configuration(), backend };
        (void)session.Attach(ClientA, Generation, 0, 1);
        (void)session.Pull(ClientA);
        (void)session.Attach(ClientB, Generation, 2, 1);
        const auto acquired = session.AcquireInputLease(ClientA, 10);
        VERIFY_IS_TRUE(acquired.Lease.has_value());
        if (!acquired.Lease)
        {
            return;
        }
        const auto revision = acquired.Lease->LeaseRevision;

        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(10, { 'a', 'b' }),
                11,
                revision).Status == RuntimeTerminalSessionStatus::InputAccepted);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Resize(11, 120, 40),
                12,
                revision).Status == RuntimeTerminalSessionStatus::ResizeAccepted);
        VERIFY_ARE_EQUAL(1u, backend->WriteInputCalls);
        VERIFY_ARE_EQUAL(1u, backend->ResizeCalls);
        VERIFY_IS_TRUE((
            backend->Inputs.front() == std::vector<uint8_t>{ 'a', 'b' }));
        VERIFY_IS_TRUE((
            backend->Sizes.front() == std::pair<uint32_t, uint32_t>{ 120, 40 }));

        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientB,
                Input(12, { 'x' }),
                13,
                std::nullopt).Status == RuntimeTerminalSessionStatus::NoLease);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientB,
                Resize(13, 80, 24),
                13,
                revision).Status == RuntimeTerminalSessionStatus::NoLease);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(14, { 'x' }),
                13,
                std::nullopt).Status == RuntimeTerminalSessionStatus::StaleLease);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(15, { 'x' }, Generation + 1),
                13,
                revision).Status == RuntimeTerminalSessionStatus::StaleGeneration);
        VERIFY_ARE_EQUAL(1u, backend->WriteInputCalls);
        VERIFY_ARE_EQUAL(1u, backend->ResizeCalls);
    }

    void RuntimeTerminalSessionFrameTests::EscapeAndControlStopConvergeOnBackendInterrupt()
    {
        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        RuntimeTerminalSession session{ Configuration(), backend };
        (void)session.Attach(ClientA, Generation, 0, 1);
        (void)session.Pull(ClientA);
        const auto acquired = session.AcquireInputLease(ClientA, 10);
        VERIFY_IS_TRUE(acquired.Lease.has_value());
        if (!acquired.Lease)
        {
            return;
        }
        const auto revision = acquired.Lease->LeaseRevision;

        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(10, { 0x1b }),
                11,
                revision).Status == RuntimeTerminalSessionStatus::Interrupted);
        VERIFY_IS_TRUE(
            session.Stop().Status == RuntimeTerminalSessionStatus::Interrupted);
        VERIFY_ARE_EQUAL(2u, backend->InterruptCalls);
        VERIFY_ARE_EQUAL(0u, backend->WriteInputCalls);

        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(11, { 0x1b, 'x' }),
                12,
                revision).Status == RuntimeTerminalSessionStatus::InputAccepted);
        VERIFY_ARE_EQUAL(2u, backend->InterruptCalls);
        VERIFY_ARE_EQUAL(1u, backend->WriteInputCalls);
    }

    void RuntimeTerminalSessionFrameTests::SnapshotFailuresDetachOnlyTheAttachingClient()
    {
        auto rejected = std::make_shared<FakeRuntimeTerminalBackend>();
        rejected->SnapshotStatus = RuntimeTerminalBackendStatus::Rejected;
        rejected->SnapshotError = "snapshot rejected";
        RuntimeTerminalSession rejectedSession{ Configuration(), rejected };
        const auto rejectedAttach = rejectedSession.Attach(ClientA, Generation, 0, 1);
        VERIFY_IS_TRUE(
            rejectedAttach.Status == RuntimeTerminalSessionStatus::BackendFailure);
        VERIFY_ARE_EQUAL(std::string{ "snapshot rejected" }, rejectedAttach.Error);
        VERIFY_IS_TRUE((
            rejectedAttach.DisconnectedClients ==
            std::vector<std::string>{ std::string{ ClientA } }));
        VERIFY_ARE_EQUAL(size_t{ 0 }, rejectedSession.ClientCount());

        auto throwing = std::make_shared<FakeRuntimeTerminalBackend>();
        throwing->ThrowOnSnapshot = true;
        RuntimeTerminalSession throwingSession{ Configuration(), throwing };
        const auto throwingAttach = throwingSession.Attach(ClientB, Generation, 0, 1);
        VERIFY_IS_TRUE(
            throwingAttach.Status == RuntimeTerminalSessionStatus::BackendFailure);
        VERIFY_ARE_EQUAL(size_t{ 0 }, throwingSession.ClientCount());

        auto empty = std::make_shared<FakeRuntimeTerminalBackend>();
        empty->SnapshotPayload.clear();
        RuntimeTerminalSession emptySession{ Configuration(), empty };
        VERIFY_IS_TRUE(
            emptySession.Attach(ClientC, Generation, 0, 1).Status ==
            RuntimeTerminalSessionStatus::BackendFailure);
        VERIFY_ARE_EQUAL(size_t{ 0 }, emptySession.ClientCount());
    }

    void RuntimeTerminalSessionFrameTests::BackendOperationFailuresAreContainedAndReported()
    {
        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        RuntimeTerminalSession session{ Configuration(), backend };
        (void)session.Attach(ClientA, Generation, 0, 1);
        (void)session.Pull(ClientA);
        const auto acquired = session.AcquireInputLease(ClientA, 10);
        VERIFY_IS_TRUE(acquired.Lease.has_value());
        if (!acquired.Lease)
        {
            return;
        }
        const auto revision = acquired.Lease->LeaseRevision;

        backend->WriteInputStatus = RuntimeTerminalBackendStatus::Rejected;
        backend->WriteInputError = "input rejected";
        const auto rejectedInput = session.HandleFrame(
            ClientA,
            Input(10, { 'x' }),
            11,
            revision);
        VERIFY_IS_TRUE(
            rejectedInput.Status == RuntimeTerminalSessionStatus::BackendFailure);
        VERIFY_ARE_EQUAL(std::string{ "input rejected" }, rejectedInput.Error);

        backend->ThrowOnWriteInput = true;
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(11, { 'y' }),
                12,
                revision).Status == RuntimeTerminalSessionStatus::BackendFailure);

        backend->ResizeStatus = RuntimeTerminalBackendStatus::Closed;
        backend->ResizeError = "resize closed";
        const auto closedResize = session.HandleFrame(
            ClientA,
            Resize(12, 100, 30),
            13,
            revision);
        VERIFY_IS_TRUE(
            closedResize.Status == RuntimeTerminalSessionStatus::BackendFailure);
        VERIFY_ARE_EQUAL(std::string{ "resize closed" }, closedResize.Error);

        backend->ThrowOnResize = true;
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Resize(13, 101, 31),
                14,
                revision).Status == RuntimeTerminalSessionStatus::BackendFailure);

        backend->InterruptStatus = RuntimeTerminalBackendStatus::SystemError;
        backend->InterruptError = "interrupt failed";
        const auto failedStop = session.Stop();
        VERIFY_IS_TRUE(failedStop.Status == RuntimeTerminalSessionStatus::BackendFailure);
        VERIFY_ARE_EQUAL(std::string{ "interrupt failed" }, failedStop.Error);

        backend->ThrowOnInterrupt = true;
        VERIFY_IS_TRUE(
            session.Stop().Status == RuntimeTerminalSessionStatus::BackendFailure);
        VERIFY_ARE_EQUAL(2u, backend->WriteInputCalls);
        VERIFY_ARE_EQUAL(2u, backend->ResizeCalls);
        VERIFY_ARE_EQUAL(2u, backend->InterruptCalls);
    }

    void RuntimeTerminalSessionFrameTests::HeartbeatValidationAndSequenceExhaustionFailClosed()
    {
        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        RuntimeTerminalSession session{ Configuration(), backend };
        (void)session.Attach(ClientA, Generation, 0, 1);
        (void)session.Pull(ClientA);

        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Ping(10),
                2,
                std::nullopt).Status == RuntimeTerminalSessionStatus::PongQueued);
        const auto pong = session.Pull(ClientA);
        VERIFY_IS_TRUE(pong.Frame.has_value());
        if (!pong.Frame)
        {
            return;
        }
        VERIFY_IS_TRUE(pong.Frame->Kind == TerminalFrameKind::Pong);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, pong.Frame->Sequence);
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, pong.Frame->Generation);

        auto unsupported = Input(11, { 'x' });
        unsupported.Kind = TerminalFrameKind::Output;
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                unsupported,
                3,
                std::nullopt).Status == RuntimeTerminalSessionStatus::InvalidFrame);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientA,
                Input(12, {}),
                3,
                std::nullopt).Status == RuntimeTerminalSessionStatus::InvalidFrame);
        VERIFY_IS_TRUE(
            session.HandleFrame(
                ClientB,
                Ping(13),
                3,
                std::nullopt).Status == RuntimeTerminalSessionStatus::NotAttached);

        auto exhaustedConfiguration = Configuration();
        exhaustedConfiguration.InitialServerSequence =
            (std::numeric_limits<uint64_t>::max)() - 1;
        auto exhaustedBackend = std::make_shared<FakeRuntimeTerminalBackend>();
        RuntimeTerminalSession exhausted{
            exhaustedConfiguration,
            exhaustedBackend,
        };
        VERIFY_IS_TRUE(exhausted.Valid());
        VERIFY_IS_TRUE(
            exhausted.Attach(ClientA, Generation, 0, 1).Status ==
            RuntimeTerminalSessionStatus::ResetQueued);
        const auto maximumFrame = exhausted.Pull(ClientA);
        VERIFY_IS_TRUE(maximumFrame.Frame.has_value());
        if (!maximumFrame.Frame)
        {
            return;
        }
        VERIFY_ARE_EQUAL(
            (std::numeric_limits<uint64_t>::max)(),
            maximumFrame.Frame->Sequence);
        VERIFY_IS_TRUE(
            exhausted.HandleFrame(
                ClientA,
                Ping(14),
                2,
                std::nullopt).Status == RuntimeTerminalSessionStatus::SequenceExhausted);
        VERIFY_IS_TRUE(
            exhausted.PublishOutput({ 'x' }, 0, 2).Status ==
            RuntimeTerminalSessionStatus::SequenceExhausted);

        auto clockBackend = std::make_shared<FakeRuntimeTerminalBackend>();
        RuntimeTerminalSession clockExhausted{ Configuration(), clockBackend };
        (void)clockExhausted.Attach(ClientA, Generation, 0, 1);
        VERIFY_IS_TRUE(
            clockExhausted.AcquireInputLease(
                ClientA,
                (std::numeric_limits<uint64_t>::max)()).Status ==
            RuntimeTerminalSessionStatus::LeaseClockExhausted);
    }

    void RuntimeTerminalSessionFrameTests::InvalidOutputDoesNotAdvanceAndEndOfStreamIsPublished()
    {
        auto backend = std::make_shared<FakeRuntimeTerminalBackend>();
        RuntimeTerminalSession session{ Configuration(), backend };
        (void)session.Attach(ClientA, Generation, 0, 1);
        (void)session.Pull(ClientA);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, session.LatestOutputSequence());

        VERIFY_IS_TRUE(
            session.PublishOutput({}, 0, 2).Status ==
            RuntimeTerminalSessionStatus::InvalidFrame);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, session.LatestOutputSequence());

        const auto endFlag = static_cast<uint32_t>(TerminalFrameFlag::EndOfStream);
        VERIFY_IS_TRUE(
            session.PublishOutput({}, endFlag, 3).Status ==
            RuntimeTerminalSessionStatus::OutputPublished);
        const auto end = session.Pull(ClientA);
        VERIFY_IS_TRUE(end.Frame.has_value());
        if (!end.Frame)
        {
            return;
        }
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, end.Frame->Sequence);
        VERIFY_ARE_EQUAL(endFlag, end.Frame->Flags);
        VERIFY_IS_TRUE(end.Frame->Payload.empty());
    }
}
