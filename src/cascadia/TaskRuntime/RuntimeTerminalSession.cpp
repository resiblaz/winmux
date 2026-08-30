// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalSession.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        RuntimeTerminalOutputRingConfiguration _ringConfiguration(
            const RuntimeTerminalSessionConfiguration& configuration)
        {
            return {
                configuration.Generation,
                configuration.MaximumOutputRingBytes,
                configuration.MaximumOutputRingFrames,
                configuration.MaximumFrameBytes,
            };
        }

        RuntimeTerminalInputLeaseConfiguration _leaseConfiguration(
            const RuntimeTerminalSessionConfiguration& configuration)
        {
            return {
                configuration.Generation,
                configuration.LeaseDurationMilliseconds,
                0,
            };
        }

        RuntimeTerminalClientRegistryConfiguration _clientConfiguration(
            const RuntimeTerminalSessionConfiguration& configuration)
        {
            return {
                configuration.Generation,
                configuration.MaximumClients,
                configuration.MaximumQueuedBytesPerClient,
                configuration.MaximumQueuedFramesPerClient,
                configuration.MaximumFrameBytes,
            };
        }
    }

    RuntimeTerminalSession::RuntimeTerminalSession(
        RuntimeTerminalSessionConfiguration configuration,
        std::shared_ptr<IRuntimeTerminalBackend> backend) noexcept :
        _configuration{ std::move(configuration) },
        _backend{ std::move(backend) },
        _output{ _ringConfiguration(_configuration) },
        _lease{ _leaseConfiguration(_configuration) },
        _clients{ _clientConfiguration(_configuration) },
        _lastServerSequence{ _configuration.InitialServerSequence }
    {
        _valid = _backend && _output.Valid() && _lease.Valid() &&
                 _clients.Valid() && _configuration.InitialServerSequence != 0 &&
                 _configuration.InitialServerSequence <
                     (std::numeric_limits<uint64_t>::max)();
    }

    bool RuntimeTerminalSession::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::Attach(
        const std::string_view clientInstanceId,
        const uint64_t generation,
        const uint64_t acknowledgedOutputSequence,
        const RuntimeTerminalLeaseMilliseconds now)
    {
        RuntimeTerminalSessionResult result;
        if (!_valid)
        {
            return result;
        }
        const auto attached = _clients.Attach(
            clientInstanceId,
            generation,
            acknowledgedOutputSequence);
        switch (attached.Status)
        {
        case RuntimeTerminalClientRegistryStatus::Attached:
            break;
        case RuntimeTerminalClientRegistryStatus::AlreadyAttached:
            result.Status = RuntimeTerminalSessionStatus::AlreadyAttached;
            return result;
        case RuntimeTerminalClientRegistryStatus::StaleGeneration:
            result.Status = RuntimeTerminalSessionStatus::StaleGeneration;
            return result;
        case RuntimeTerminalClientRegistryStatus::CapacityExceeded:
            result.Status = RuntimeTerminalSessionStatus::CapacityExceeded;
            return result;
        case RuntimeTerminalClientRegistryStatus::ResourceFailure:
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            return result;
        default:
            result.Status = RuntimeTerminalSessionStatus::InvalidConfiguration;
            return result;
        }

        const auto catchUp = _output.CatchUp(
            generation,
            acknowledgedOutputSequence);
        if (catchUp.Status == RuntimeTerminalCatchUpStatus::Replay)
        {
            const auto queued = _clients.QueueMany(clientInstanceId, catchUp.Frames);
            if (queued.Status == RuntimeTerminalClientRegistryStatus::Queued)
            {
                result.Status = RuntimeTerminalSessionStatus::ReplayQueued;
                return result;
            }
            return _disconnectClient(
                clientInstanceId,
                now,
                queued.Status == RuntimeTerminalClientRegistryStatus::SlowPeer ?
                    RuntimeTerminalSessionStatus::SlowPeerDisconnected :
                    RuntimeTerminalSessionStatus::ResourceFailure);
        }
        if (catchUp.Status == RuntimeTerminalCatchUpStatus::ResetRequired ||
            (catchUp.Status == RuntimeTerminalCatchUpStatus::UpToDate &&
             _output.LatestSequence() == 0))
        {
            return _queueSnapshot(clientInstanceId, now);
        }
        if (catchUp.Status == RuntimeTerminalCatchUpStatus::UpToDate)
        {
            result.Status = RuntimeTerminalSessionStatus::Attached;
            return result;
        }

        (void)_clients.Detach(clientInstanceId);
        result.Status = catchUp.Status ==
                                RuntimeTerminalCatchUpStatus::StaleGeneration ?
                            RuntimeTerminalSessionStatus::StaleGeneration :
                        catchUp.Status ==
                                RuntimeTerminalCatchUpStatus::AcknowledgementAhead ?
                            RuntimeTerminalSessionStatus::AcknowledgementAhead :
                            RuntimeTerminalSessionStatus::ResourceFailure;
        return result;
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::Detach(
        const std::string_view clientInstanceId,
        const RuntimeTerminalLeaseMilliseconds now)
    {
        RuntimeTerminalSessionResult result;
        const auto detached = _clients.Detach(clientInstanceId);
        if (detached.Status == RuntimeTerminalClientRegistryStatus::NotFound)
        {
            result.Status = RuntimeTerminalSessionStatus::NotAttached;
            return result;
        }
        if (detached.Status != RuntimeTerminalClientRegistryStatus::Detached)
        {
            return result;
        }
        (void)_lease.Disconnect(clientInstanceId, now);
        result.Status = RuntimeTerminalSessionStatus::Detached;
        return result;
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::PublishOutput(
        std::vector<uint8_t> payload,
        const uint32_t flags,
        const RuntimeTerminalLeaseMilliseconds now)
    {
        RuntimeTerminalSessionResult result;
        if (!_valid)
        {
            return result;
        }
        std::vector<std::string> clientIds;
        try
        {
            clientIds = _clients.ClientIds();
            result.DisconnectedClients.reserve(clientIds.size());
        }
        catch (...)
        {
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            return result;
        }
        const auto sequence = _nextSequence();
        if (!sequence)
        {
            result.Status = RuntimeTerminalSessionStatus::SequenceExhausted;
            return result;
        }

        TerminalFrame frame;
        frame.Kind = TerminalFrameKind::Output;
        frame.Flags = flags;
        frame.Sequence = *sequence;
        frame.Generation = _configuration.Generation;
        frame.Payload = std::move(payload);
        RuntimeTerminalOutputAppendResult appended;
        try
        {
            appended = _output.Append(frame);
        }
        catch (...)
        {
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            return result;
        }
        if (appended.Status != RuntimeTerminalOutputAppendStatus::Added &&
            appended.Status != RuntimeTerminalOutputAppendStatus::AddedWithEviction)
        {
            result.Status = appended.Status ==
                                    RuntimeTerminalOutputAppendStatus::ResourceFailure ?
                                RuntimeTerminalSessionStatus::ResourceFailure :
                                RuntimeTerminalSessionStatus::InvalidFrame;
            return result;
        }
        _lastServerSequence = *sequence;

        for (const auto& clientId : clientIds)
        {
            const auto queued = _clients.Queue(clientId, frame);
            if (queued.Status != RuntimeTerminalClientRegistryStatus::Queued)
            {
                (void)_lease.Disconnect(clientId, now);
                (void)_clients.Detach(clientId);
                try
                {
                    result.DisconnectedClients.emplace_back(clientId);
                }
                catch (...)
                {
                    result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
                }
                if (queued.Status != RuntimeTerminalClientRegistryStatus::SlowPeer)
                {
                    result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
                }
            }
        }
        if (result.Status != RuntimeTerminalSessionStatus::ResourceFailure)
        {
            result.Status = result.DisconnectedClients.empty() ?
                                RuntimeTerminalSessionStatus::OutputPublished :
                                RuntimeTerminalSessionStatus::OutputPublishedWithSlowPeerDisconnect;
        }
        return result;
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::Pull(
        const std::string_view clientInstanceId) noexcept
    {
        RuntimeTerminalSessionResult result;
        const auto pulled = _clients.Pull(clientInstanceId);
        if (pulled.Status == RuntimeTerminalClientRegistryStatus::Pulled)
        {
            result.Status = RuntimeTerminalSessionStatus::FramePulled;
            result.Frame = std::move(pulled.Frame);
        }
        else if (pulled.Status == RuntimeTerminalClientRegistryStatus::Empty)
        {
            result.Status = RuntimeTerminalSessionStatus::Empty;
        }
        else if (pulled.Status == RuntimeTerminalClientRegistryStatus::NotFound)
        {
            result.Status = RuntimeTerminalSessionStatus::NotAttached;
        }
        return result;
    }

    size_t RuntimeTerminalSession::ClientCount() const noexcept
    {
        return _clients.Size();
    }

    uint64_t RuntimeTerminalSession::LatestOutputSequence() const noexcept
    {
        return _output.LatestSequence();
    }

    std::optional<RuntimeTerminalInputLeaseSnapshot> RuntimeTerminalSession::CurrentLease() const
    {
        return _lease.Current();
    }

    std::optional<uint64_t> RuntimeTerminalSession::_nextSequence() noexcept
    {
        return _lastServerSequence == (std::numeric_limits<uint64_t>::max)() ?
                   std::nullopt :
                   std::optional<uint64_t>{ _lastServerSequence + 1 };
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::_queueSnapshot(
        const std::string_view clientInstanceId,
        const RuntimeTerminalLeaseMilliseconds now)
    {
        RuntimeTerminalBackendSnapshotResult snapshot;
        try
        {
            snapshot = _backend->Snapshot();
        }
        catch (...)
        {
            return _disconnectClient(
                clientInstanceId,
                now,
                RuntimeTerminalSessionStatus::BackendFailure);
        }
        if (!snapshot.Succeeded() || snapshot.Payload.empty())
        {
            auto result = _disconnectClient(
                clientInstanceId,
                now,
                RuntimeTerminalSessionStatus::BackendFailure);
            result.Error = snapshot.Error;
            return result;
        }
        const auto sequence = _nextSequence();
        if (!sequence)
        {
            return _disconnectClient(
                clientInstanceId,
                now,
                RuntimeTerminalSessionStatus::SequenceExhausted);
        }
        TerminalFrame frame;
        frame.Kind = TerminalFrameKind::Output;
        frame.Flags = static_cast<uint32_t>(
            TerminalFrameFlag::ResetBeforePayload);
        frame.Sequence = *sequence;
        frame.Generation = _configuration.Generation;
        frame.Payload = std::move(snapshot.Payload);
        RuntimeTerminalOutputAppendResult appended;
        try
        {
            appended = _output.Append(frame);
        }
        catch (...)
        {
            return _disconnectClient(
                clientInstanceId,
                now,
                RuntimeTerminalSessionStatus::ResourceFailure);
        }
        if (appended.Status != RuntimeTerminalOutputAppendStatus::Added &&
            appended.Status != RuntimeTerminalOutputAppendStatus::AddedWithEviction)
        {
            return _disconnectClient(
                clientInstanceId,
                now,
                appended.Status == RuntimeTerminalOutputAppendStatus::ResourceFailure ?
                    RuntimeTerminalSessionStatus::ResourceFailure :
                    RuntimeTerminalSessionStatus::InvalidFrame);
        }
        _lastServerSequence = *sequence;
        const auto queued = _clients.Queue(clientInstanceId, frame);
        if (queued.Status != RuntimeTerminalClientRegistryStatus::Queued)
        {
            return _disconnectClient(
                clientInstanceId,
                now,
                queued.Status == RuntimeTerminalClientRegistryStatus::SlowPeer ?
                    RuntimeTerminalSessionStatus::SlowPeerDisconnected :
                    RuntimeTerminalSessionStatus::ResourceFailure);
        }
        RuntimeTerminalSessionResult result;
        result.Status = RuntimeTerminalSessionStatus::ResetQueued;
        return result;
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::_disconnectClient(
        const std::string_view clientInstanceId,
        const RuntimeTerminalLeaseMilliseconds now,
        const RuntimeTerminalSessionStatus status)
    {
        RuntimeTerminalSessionResult result;
        (void)_lease.Disconnect(clientInstanceId, now);
        (void)_clients.Detach(clientInstanceId);
        result.Status = status;
        try
        {
            result.DisconnectedClients.emplace_back(clientInstanceId);
        }
        catch (...)
        {
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
        }
        return result;
    }
}
