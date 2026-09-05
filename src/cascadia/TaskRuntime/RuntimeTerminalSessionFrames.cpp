// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalSession.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    RuntimeTerminalSessionResult RuntimeTerminalSession::AcquireInputLease(
        const std::string_view clientInstanceId,
        const RuntimeTerminalLeaseMilliseconds now)
    {
        RuntimeTerminalSessionResult result;
        if (!_valid)
        {
            return result;
        }
        try
        {
            if (!_clients.Find(clientInstanceId))
            {
                result.Status = RuntimeTerminalSessionStatus::NotAttached;
                return result;
            }
        }
        catch (...)
        {
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            return result;
        }

        auto acquired = _lease.Acquire(
            clientInstanceId,
            _configuration.Generation,
            now);
        result.Lease = std::move(acquired.Lease);
        if (acquired.Status != RuntimeTerminalInputLeaseStatus::Acquired &&
            acquired.Status != RuntimeTerminalInputLeaseStatus::Renewed)
        {
            return _leaseFailure(acquired.Status);
        }
        if (!result.Lease)
        {
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            return result;
        }

        try
        {
            for (const auto& clientId : _clients.ClientIds())
            {
                if (clientId != clientInstanceId)
                {
                    (void)_clients.ClearLease(clientId);
                }
            }
        }
        catch (...)
        {
            (void)_lease.Release(
                clientInstanceId,
                _configuration.Generation,
                result.Lease->LeaseRevision,
                now);
            result.Lease.reset();
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            return result;
        }
        const auto bound = _clients.BindLease(
            clientInstanceId,
            result.Lease->LeaseRevision);
        if (bound.Status != RuntimeTerminalClientRegistryStatus::LeaseBound)
        {
            (void)_lease.Release(
                clientInstanceId,
                _configuration.Generation,
                result.Lease->LeaseRevision,
                now);
            result.Lease.reset();
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            return result;
        }
        result.Status = acquired.Status == RuntimeTerminalInputLeaseStatus::Acquired ?
                            RuntimeTerminalSessionStatus::LeaseAcquired :
                            RuntimeTerminalSessionStatus::LeaseRenewed;
        return result;
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::ReleaseInputLease(
        const std::string_view clientInstanceId,
        const RuntimeTerminalLeaseRevision leaseRevision,
        const RuntimeTerminalLeaseMilliseconds now)
    {
        RuntimeTerminalSessionResult result;
        std::optional<RuntimeTerminalClientSnapshot> client;
        try
        {
            client = _clients.Find(clientInstanceId);
        }
        catch (...)
        {
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            return result;
        }
        if (!client)
        {
            result.Status = RuntimeTerminalSessionStatus::NotAttached;
            return result;
        }
        if (!client->LeaseRevision)
        {
            result.Status = RuntimeTerminalSessionStatus::NoLease;
            return result;
        }
        auto released = _lease.Release(
            clientInstanceId,
            _configuration.Generation,
            leaseRevision,
            now);
        result.Lease = std::move(released.Lease);
        if (released.Status == RuntimeTerminalInputLeaseStatus::Released)
        {
            (void)_clients.ClearLease(clientInstanceId);
            result.Status = RuntimeTerminalSessionStatus::LeaseReleased;
            return result;
        }
        if (released.Status == RuntimeTerminalInputLeaseStatus::Expired ||
            released.Status == RuntimeTerminalInputLeaseStatus::AlreadyReleased)
        {
            (void)_clients.ClearLease(clientInstanceId);
        }
        return _leaseFailure(released.Status);
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::HandleFrame(
        const std::string_view clientInstanceId,
        const TerminalFrame& frame,
        const RuntimeTerminalLeaseMilliseconds now,
        const std::optional<RuntimeTerminalLeaseRevision> inputLeaseRevision)
    {
        return DispatchFrame(frame, PrepareFrame(clientInstanceId, frame, now, inputLeaseRevision));
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::PrepareFrame(
        const std::string_view clientInstanceId,
        const TerminalFrame& frame,
        const RuntimeTerminalLeaseMilliseconds now,
        const std::optional<RuntimeTerminalLeaseRevision> inputLeaseRevision)
    {
        RuntimeTerminalSessionResult result;
        if (!_valid)
        {
            return result;
        }
        std::string validationError;
        if (!ValidateTerminalFrame(
                frame,
                _configuration.MaximumFrameBytes,
                validationError))
        {
            result.Status = RuntimeTerminalSessionStatus::InvalidFrame;
            result.Error = std::move(validationError);
            return result;
        }
        if (frame.Generation != 0 &&
            frame.Generation != _configuration.Generation)
        {
            result.Status = RuntimeTerminalSessionStatus::StaleGeneration;
            return result;
        }

        std::optional<RuntimeTerminalClientSnapshot> client;
        try
        {
            client = _clients.Find(clientInstanceId);
        }
        catch (...)
        {
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            return result;
        }
        if (!client)
        {
            result.Status = RuntimeTerminalSessionStatus::NotAttached;
            return result;
        }

        switch (frame.Kind)
        {
        case TerminalFrameKind::Acknowledge:
        {
            uint64_t acknowledged{};
            if (!DecodeAcknowledgementPayload(frame.Payload, acknowledged))
            {
                result.Status = RuntimeTerminalSessionStatus::InvalidFrame;
                return result;
            }
            const auto acknowledgement = _clients.Acknowledge(
                clientInstanceId,
                _configuration.Generation,
                acknowledged,
                _output.LatestSequence());
            switch (acknowledgement.Status)
            {
            case RuntimeTerminalClientRegistryStatus::Acknowledged:
                result.Status = RuntimeTerminalSessionStatus::Acknowledged;
                break;
            case RuntimeTerminalClientRegistryStatus::DuplicateAcknowledgement:
                result.Status = RuntimeTerminalSessionStatus::DuplicateAcknowledgement;
                break;
            case RuntimeTerminalClientRegistryStatus::AcknowledgementAhead:
                result.Status = RuntimeTerminalSessionStatus::AcknowledgementAhead;
                break;
            case RuntimeTerminalClientRegistryStatus::AcknowledgementRegression:
                result.Status = RuntimeTerminalSessionStatus::AcknowledgementRegression;
                break;
            default:
                result.Status = RuntimeTerminalSessionStatus::InvalidFrame;
                break;
            }
            return result;
        }
        case TerminalFrameKind::Input:
        case TerminalFrameKind::Resize:
        {
            if (!client->LeaseRevision)
            {
                result.Status = RuntimeTerminalSessionStatus::NoLease;
                return result;
            }
            if (!inputLeaseRevision)
            {
                result.Status = RuntimeTerminalSessionStatus::StaleLease;
                return result;
            }
            const auto authorized = _lease.AuthorizeInput(
                clientInstanceId,
                _configuration.Generation,
                *inputLeaseRevision,
                now);
            if (authorized.Status != RuntimeTerminalInputLeaseStatus::Authorized)
            {
                if (authorized.Status == RuntimeTerminalInputLeaseStatus::Expired)
                {
                    (void)_clients.ClearLease(clientInstanceId);
                }
                return _leaseFailure(authorized.Status);
            }
            if (frame.Kind == TerminalFrameKind::Input)
            {
                if (frame.Payload.size() == 1 && frame.Payload.front() == 0x1b)
                {
                    result.Status = RuntimeTerminalSessionStatus::Interrupted;
                }
                else
                {
                    result.Status = RuntimeTerminalSessionStatus::InputAccepted;
                }
                return result;
            }
            uint32_t columns{};
            uint32_t rows{};
            if (!DecodeResizePayload(frame.Payload, columns, rows))
            {
                result.Status = RuntimeTerminalSessionStatus::InvalidFrame;
                return result;
            }
            result.Status = RuntimeTerminalSessionStatus::ResizeAccepted;
            return result;
        }
        case TerminalFrameKind::Ping:
        {
            const auto sequence = _nextSequence();
            if (!sequence)
            {
                result.Status = RuntimeTerminalSessionStatus::SequenceExhausted;
                return result;
            }
            TerminalFrame pong;
            pong.Kind = TerminalFrameKind::Pong;
            pong.Sequence = *sequence;
            pong.Generation = 0;
            const auto queued = _clients.Queue(clientInstanceId, pong);
            if (queued.Status != RuntimeTerminalClientRegistryStatus::Queued)
            {
                return _disconnectClient(
                    clientInstanceId,
                    now,
                    queued.Status == RuntimeTerminalClientRegistryStatus::SlowPeer ?
                        RuntimeTerminalSessionStatus::SlowPeerDisconnected :
                        RuntimeTerminalSessionStatus::ResourceFailure);
            }
            _lastServerSequence = *sequence;
            result.Status = RuntimeTerminalSessionStatus::PongQueued;
            return result;
        }
        default:
            result.Status = RuntimeTerminalSessionStatus::InvalidFrame;
            return result;
        }
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::DispatchFrame(
        const TerminalFrame& frame,
        RuntimeTerminalSessionResult prepared)
    {
        try
        {
            switch (prepared.Status)
            {
            case RuntimeTerminalSessionStatus::InputAccepted:
                return _backendResult(_backend->WriteInput(frame.Payload), prepared.Status);
            case RuntimeTerminalSessionStatus::Interrupted:
                return _interrupt();
            case RuntimeTerminalSessionStatus::ResizeAccepted:
            {
                uint32_t columns{};
                uint32_t rows{};
                if (!DecodeResizePayload(frame.Payload, columns, rows))
                {
                    prepared.Status = RuntimeTerminalSessionStatus::InvalidFrame;
                    return prepared;
                }
                return _backendResult(_backend->Resize(columns, rows), prepared.Status);
            }
            default:
                return prepared;
            }
        }
        catch (...)
        {
            prepared.Status = RuntimeTerminalSessionStatus::BackendFailure;
            return prepared;
        }
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::Stop()
    {
        return _interrupt();
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::_interrupt()
    {
        RuntimeTerminalSessionResult result;
        if (!_valid || !_backend)
        {
            return result;
        }
        try
        {
            return _backendResult(
                _backend->Interrupt(),
                RuntimeTerminalSessionStatus::Interrupted);
        }
        catch (...)
        {
            result.Status = RuntimeTerminalSessionStatus::BackendFailure;
            return result;
        }
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::_leaseFailure(
        const RuntimeTerminalInputLeaseStatus status) const
    {
        RuntimeTerminalSessionResult result;
        switch (status)
        {
        case RuntimeTerminalInputLeaseStatus::Busy:
            result.Status = RuntimeTerminalSessionStatus::LeaseBusy;
            break;
        case RuntimeTerminalInputLeaseStatus::NoLease:
        case RuntimeTerminalInputLeaseStatus::AlreadyReleased:
            result.Status = RuntimeTerminalSessionStatus::NoLease;
            break;
        case RuntimeTerminalInputLeaseStatus::NotOwner:
            result.Status = RuntimeTerminalSessionStatus::NotOwner;
            break;
        case RuntimeTerminalInputLeaseStatus::StaleLease:
            result.Status = RuntimeTerminalSessionStatus::StaleLease;
            break;
        case RuntimeTerminalInputLeaseStatus::StaleGeneration:
            result.Status = RuntimeTerminalSessionStatus::StaleGeneration;
            break;
        case RuntimeTerminalInputLeaseStatus::Expired:
            result.Status = RuntimeTerminalSessionStatus::LeaseExpired;
            break;
        case RuntimeTerminalInputLeaseStatus::ClockRegression:
            result.Status = RuntimeTerminalSessionStatus::LeaseClockRegression;
            break;
        case RuntimeTerminalInputLeaseStatus::ClockExhausted:
            result.Status = RuntimeTerminalSessionStatus::LeaseClockExhausted;
            break;
        case RuntimeTerminalInputLeaseStatus::RevisionExhausted:
            result.Status = RuntimeTerminalSessionStatus::LeaseRevisionExhausted;
            break;
        case RuntimeTerminalInputLeaseStatus::ResourceFailure:
            result.Status = RuntimeTerminalSessionStatus::ResourceFailure;
            break;
        default:
            result.Status = RuntimeTerminalSessionStatus::InvalidConfiguration;
            break;
        }
        return result;
    }

    RuntimeTerminalSessionResult RuntimeTerminalSession::_backendResult(
        RuntimeTerminalBackendResult backend,
        const RuntimeTerminalSessionStatus succeededStatus) const
    {
        RuntimeTerminalSessionResult result;
        result.Error = std::move(backend.Error);
        result.Status = backend.Succeeded() ?
                            succeededStatus :
                            RuntimeTerminalSessionStatus::BackendFailure;
        return result;
    }
}
