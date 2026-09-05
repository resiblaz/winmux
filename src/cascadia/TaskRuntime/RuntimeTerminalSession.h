// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeTerminalBackend.h"
#include "RuntimeTerminalClientRegistry.h"
#include "RuntimeTerminalInputLease.h"
#include "RuntimeTerminalOutputRing.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr size_t DefaultRuntimeTerminalSessionClientQueueBytes{
        DefaultRuntimeTerminalOutputRingBytes +
        DefaultRuntimeTerminalOutputRingFrames *
            Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrameHeaderBytes
    };

    struct RuntimeTerminalSessionConfiguration
    {
        uint64_t Generation{};
        uint64_t InitialServerSequence{ 1 };
        size_t MaximumOutputRingBytes{ DefaultRuntimeTerminalOutputRingBytes };
        size_t MaximumOutputRingFrames{ DefaultRuntimeTerminalOutputRingFrames };
        size_t MaximumClients{ DefaultRuntimeTerminalMaximumClients };
        size_t MaximumQueuedBytesPerClient{
            DefaultRuntimeTerminalSessionClientQueueBytes
        };
        size_t MaximumQueuedFramesPerClient{
            DefaultRuntimeTerminalClientQueueFrames
        };
        uint32_t MaximumFrameBytes{
            Microsoft::Terminal::TaskRuntimeProtocol::MaximumTerminalFrameBytes
        };
        uint32_t LeaseDurationMilliseconds{
            DefaultRuntimeTerminalLeaseDurationMilliseconds
        };
    };

    enum class RuntimeTerminalSessionStatus
    {
        Attached,
        ReplayQueued,
        ResetQueued,
        Detached,
        LeaseAcquired,
        LeaseRenewed,
        LeaseReleased,
        OutputPublished,
        OutputPublishedWithSlowPeerDisconnect,
        InputAccepted,
        ResizeAccepted,
        Acknowledged,
        DuplicateAcknowledgement,
        PongQueued,
        Interrupted,
        FramePulled,
        Empty,
        AlreadyAttached,
        NotAttached,
        CapacityExceeded,
        SlowPeerDisconnected,
        StaleGeneration,
        AcknowledgementAhead,
        AcknowledgementRegression,
        NoLease,
        NotOwner,
        StaleLease,
        LeaseBusy,
        LeaseExpired,
        LeaseClockRegression,
        LeaseClockExhausted,
        LeaseRevisionExhausted,
        InvalidFrame,
        InvalidConfiguration,
        SequenceExhausted,
        BackendFailure,
        ResourceFailure,
    };

    struct RuntimeTerminalSessionResult
    {
        RuntimeTerminalSessionStatus Status{
            RuntimeTerminalSessionStatus::InvalidConfiguration
        };
        std::optional<RuntimeTerminalInputLeaseSnapshot> Lease;
        std::optional<Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame> Frame;
        std::vector<std::string> DisconnectedClients;
        std::string Error;
    };

    // This state machine is deliberately single-owner and serialized. Runtime
    // transport workers enqueue work to their dispatcher; arbitrary Backend
    // methods are never invoked while holding a session lock.
    class RuntimeTerminalSession final
    {
    public:
        RuntimeTerminalSession(
            RuntimeTerminalSessionConfiguration configuration,
            std::shared_ptr<IRuntimeTerminalBackend> backend) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeTerminalSessionResult Attach(
            std::string_view clientInstanceId,
            uint64_t generation,
            uint64_t acknowledgedOutputSequence,
            RuntimeTerminalLeaseMilliseconds now);
        [[nodiscard]] RuntimeTerminalSessionResult Detach(
            std::string_view clientInstanceId,
            RuntimeTerminalLeaseMilliseconds now);
        [[nodiscard]] RuntimeTerminalSessionResult AcquireInputLease(
            std::string_view clientInstanceId,
            RuntimeTerminalLeaseMilliseconds now);
        [[nodiscard]] RuntimeTerminalSessionResult ReleaseInputLease(
            std::string_view clientInstanceId,
            RuntimeTerminalLeaseRevision leaseRevision,
            RuntimeTerminalLeaseMilliseconds now);
        [[nodiscard]] RuntimeTerminalSessionResult PublishOutput(
            std::vector<uint8_t> payload,
            uint32_t flags,
            RuntimeTerminalLeaseMilliseconds now);
        // The dispatcher must attach the lease revision captured when an
        // Input/Resize frame was received. Keeping it with the queued work
        // prevents an old frame from being authorized by a later lease (ABA).
        [[nodiscard]] RuntimeTerminalSessionResult HandleFrame(
            std::string_view clientInstanceId,
            const Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame& frame,
            RuntimeTerminalLeaseMilliseconds now,
            std::optional<RuntimeTerminalLeaseRevision> inputLeaseRevision);
        // Serialize preparation with session state, then dispatch the authorized
        // backend operation without holding the transport/session lock.
        [[nodiscard]] RuntimeTerminalSessionResult PrepareFrame(
            std::string_view clientInstanceId,
            const Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame& frame,
            RuntimeTerminalLeaseMilliseconds now,
            std::optional<RuntimeTerminalLeaseRevision> inputLeaseRevision);
        [[nodiscard]] RuntimeTerminalSessionResult DispatchFrame(
            const Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame& frame,
            RuntimeTerminalSessionResult prepared);
        [[nodiscard]] RuntimeTerminalSessionResult Stop();
        [[nodiscard]] RuntimeTerminalSessionResult Pull(
            std::string_view clientInstanceId) noexcept;

        [[nodiscard]] size_t ClientCount() const noexcept;
        [[nodiscard]] uint64_t LatestOutputSequence() const noexcept;
        [[nodiscard]] std::optional<RuntimeTerminalInputLeaseSnapshot> CurrentLease() const;

    private:
        [[nodiscard]] std::optional<uint64_t> _nextSequence() noexcept;
        [[nodiscard]] RuntimeTerminalSessionResult _queueSnapshot(
            std::string_view clientInstanceId,
            RuntimeTerminalLeaseMilliseconds now);
        [[nodiscard]] RuntimeTerminalSessionResult _disconnectClient(
            std::string_view clientInstanceId,
            RuntimeTerminalLeaseMilliseconds now,
            RuntimeTerminalSessionStatus status);
        [[nodiscard]] RuntimeTerminalSessionResult _interrupt();
        [[nodiscard]] RuntimeTerminalSessionResult _leaseFailure(
            RuntimeTerminalInputLeaseStatus status) const;
        [[nodiscard]] RuntimeTerminalSessionResult _backendResult(
            RuntimeTerminalBackendResult result,
            RuntimeTerminalSessionStatus succeededStatus) const;

        RuntimeTerminalSessionConfiguration _configuration;
        std::shared_ptr<IRuntimeTerminalBackend> _backend;
        RuntimeTerminalOutputRing _output;
        RuntimeTerminalInputLease _lease;
        RuntimeTerminalClientRegistry _clients;
        uint64_t _lastServerSequence{};
        bool _valid{};
    };
}
