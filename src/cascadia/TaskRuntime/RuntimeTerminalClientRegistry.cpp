// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalClientRegistry.h"

#include "ProtocolValidation.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    struct RuntimeTerminalClientRegistry::Client
    {
        std::string ClientInstanceId;
        uint64_t LastAcknowledgedOutputSequence{};
        std::optional<uint64_t> LeaseRevision;
        std::deque<TerminalFrame> Queue;
        size_t QueuedBytes{};
    };

    RuntimeTerminalClientRegistry::~RuntimeTerminalClientRegistry() = default;

    RuntimeTerminalClientRegistry::RuntimeTerminalClientRegistry(
        RuntimeTerminalClientRegistryConfiguration configuration) noexcept :
        _configuration{ std::move(configuration) }
    {
        _valid = _configuration.Generation != 0 &&
                 _configuration.MaximumClients != 0 &&
                 _configuration.MaximumQueuedBytesPerClient != 0 &&
                 _configuration.MaximumQueuedFramesPerClient != 0 &&
                 _configuration.MaximumFrameBytes != 0 &&
                 _configuration.MaximumFrameBytes <= MaximumTerminalFrameBytes;
        if (_valid)
        {
            try
            {
                _clients.reserve(_configuration.MaximumClients);
            }
            catch (...)
            {
                _valid = false;
            }
        }
    }

    bool RuntimeTerminalClientRegistry::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeTerminalClientRegistryResult RuntimeTerminalClientRegistry::Attach(
        const std::string_view clientInstanceId,
        const uint64_t generation,
        const uint64_t acknowledgedOutputSequence)
    {
        RuntimeTerminalClientRegistryResult result;
        if (!_valid)
        {
            return result;
        }
        if (!IsCanonicalProtocolUuid(clientInstanceId))
        {
            result.Status = RuntimeTerminalClientRegistryStatus::InvalidClient;
            return result;
        }
        if (generation != _configuration.Generation)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::StaleGeneration;
            return result;
        }
        if (_find(clientInstanceId))
        {
            result.Status = RuntimeTerminalClientRegistryStatus::AlreadyAttached;
            return result;
        }
        if (_clients.size() >= _configuration.MaximumClients)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::CapacityExceeded;
            return result;
        }
        try
        {
            Client client;
            client.ClientInstanceId = clientInstanceId;
            client.LastAcknowledgedOutputSequence = acknowledgedOutputSequence;
            _clients.emplace_back(std::move(client));
            result.Status = RuntimeTerminalClientRegistryStatus::Attached;
            return result;
        }
        catch (...)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::ResourceFailure;
            return result;
        }
    }

    RuntimeTerminalClientRegistryResult RuntimeTerminalClientRegistry::Detach(
        const std::string_view clientInstanceId) noexcept
    {
        RuntimeTerminalClientRegistryResult result;
        if (!_valid)
        {
            return result;
        }
        const auto found = std::ranges::find_if(_clients, [&](const auto& client) {
            return client.ClientInstanceId == clientInstanceId;
        });
        if (found == _clients.end())
        {
            result.Status = RuntimeTerminalClientRegistryStatus::NotFound;
            return result;
        }
        result.LeaseRevision = found->LeaseRevision;
        _clients.erase(found);
        result.Status = RuntimeTerminalClientRegistryStatus::Detached;
        return result;
    }

    RuntimeTerminalClientRegistryResult RuntimeTerminalClientRegistry::BindLease(
        const std::string_view clientInstanceId,
        const uint64_t leaseRevision) noexcept
    {
        RuntimeTerminalClientRegistryResult result;
        auto client = _find(clientInstanceId);
        if (!_valid)
        {
            return result;
        }
        if (!client)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::NotFound;
            return result;
        }
        if (leaseRevision == 0)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::InvalidLeaseRevision;
            return result;
        }
        client->LeaseRevision = leaseRevision;
        result.LeaseRevision = leaseRevision;
        result.Status = RuntimeTerminalClientRegistryStatus::LeaseBound;
        return result;
    }

    RuntimeTerminalClientRegistryResult RuntimeTerminalClientRegistry::ClearLease(
        const std::string_view clientInstanceId) noexcept
    {
        RuntimeTerminalClientRegistryResult result;
        auto client = _find(clientInstanceId);
        if (!_valid)
        {
            return result;
        }
        if (!client)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::NotFound;
            return result;
        }
        result.LeaseRevision = client->LeaseRevision;
        client->LeaseRevision.reset();
        result.Status = RuntimeTerminalClientRegistryStatus::LeaseCleared;
        return result;
    }

    RuntimeTerminalClientRegistryResult RuntimeTerminalClientRegistry::Queue(
        const std::string_view clientInstanceId,
        const TerminalFrame& frame)
    {
        return QueueMany(clientInstanceId, std::span{ &frame, size_t{ 1 } });
    }

    RuntimeTerminalClientRegistryResult RuntimeTerminalClientRegistry::QueueMany(
        const std::string_view clientInstanceId,
        const std::span<const TerminalFrame> frames)
    {
        RuntimeTerminalClientRegistryResult result;
        auto client = _find(clientInstanceId);
        if (!_valid)
        {
            return result;
        }
        if (!client)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::NotFound;
            return result;
        }
        size_t additionalBytes{};
        for (const auto& frame : frames)
        {
            if (!_validFrame(frame))
            {
                result.Status = RuntimeTerminalClientRegistryStatus::InvalidFrame;
                return result;
            }
            const auto frameBytes = _wireBytes(frame);
            if (frameBytes > (std::numeric_limits<size_t>::max)() - additionalBytes)
            {
                result.Status = RuntimeTerminalClientRegistryStatus::ResourceFailure;
                return result;
            }
            additionalBytes += frameBytes;
        }
        if (frames.size() > _configuration.MaximumQueuedFramesPerClient -
                                (std::min)(
                                    client->Queue.size(),
                                    _configuration.MaximumQueuedFramesPerClient) ||
            additionalBytes > _configuration.MaximumQueuedBytesPerClient -
                                  (std::min)(
                                      client->QueuedBytes,
                                      _configuration.MaximumQueuedBytesPerClient))
        {
            result.Status = RuntimeTerminalClientRegistryStatus::SlowPeer;
            return result;
        }
        try
        {
            for (const auto& frame : frames)
            {
                client->Queue.emplace_back(frame);
                client->QueuedBytes += _wireBytes(frame);
            }
            result.Status = RuntimeTerminalClientRegistryStatus::Queued;
            return result;
        }
        catch (...)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::ResourceFailure;
            return result;
        }
    }

    RuntimeTerminalClientRegistryResult RuntimeTerminalClientRegistry::Pull(
        const std::string_view clientInstanceId) noexcept
    {
        RuntimeTerminalClientRegistryResult result;
        auto client = _find(clientInstanceId);
        if (!_valid)
        {
            return result;
        }
        if (!client)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::NotFound;
            return result;
        }
        if (client->Queue.empty())
        {
            result.Status = RuntimeTerminalClientRegistryStatus::Empty;
            return result;
        }
        const auto frameBytes = _wireBytes(client->Queue.front());
        result.Frame = std::move(client->Queue.front());
        client->Queue.pop_front();
        client->QueuedBytes -= frameBytes;
        result.Status = RuntimeTerminalClientRegistryStatus::Pulled;
        return result;
    }

    RuntimeTerminalClientRegistryResult RuntimeTerminalClientRegistry::Acknowledge(
        const std::string_view clientInstanceId,
        const uint64_t generation,
        const uint64_t acknowledgedOutputSequence,
        const uint64_t latestOutputSequence) noexcept
    {
        RuntimeTerminalClientRegistryResult result;
        auto client = _find(clientInstanceId);
        if (!_valid)
        {
            return result;
        }
        if (!client)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::NotFound;
            return result;
        }
        if (generation != _configuration.Generation)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::StaleGeneration;
            return result;
        }
        if (acknowledgedOutputSequence > latestOutputSequence)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::AcknowledgementAhead;
            return result;
        }
        if (acknowledgedOutputSequence < client->LastAcknowledgedOutputSequence)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::AcknowledgementRegression;
            return result;
        }
        if (acknowledgedOutputSequence == client->LastAcknowledgedOutputSequence)
        {
            result.Status = RuntimeTerminalClientRegistryStatus::DuplicateAcknowledgement;
            return result;
        }
        client->LastAcknowledgedOutputSequence = acknowledgedOutputSequence;
        result.Status = RuntimeTerminalClientRegistryStatus::Acknowledged;
        return result;
    }

    std::optional<RuntimeTerminalClientSnapshot> RuntimeTerminalClientRegistry::Find(
        const std::string_view clientInstanceId) const
    {
        const auto client = _find(clientInstanceId);
        if (!client)
        {
            return std::nullopt;
        }
        RuntimeTerminalClientSnapshot snapshot;
        snapshot.ClientInstanceId = client->ClientInstanceId;
        snapshot.Generation = _configuration.Generation;
        snapshot.LastAcknowledgedOutputSequence =
            client->LastAcknowledgedOutputSequence;
        snapshot.LeaseRevision = client->LeaseRevision;
        snapshot.QueuedFrames = client->Queue.size();
        snapshot.QueuedBytes = client->QueuedBytes;
        return snapshot;
    }

    std::vector<std::string> RuntimeTerminalClientRegistry::ClientIds() const
    {
        std::vector<std::string> result;
        result.reserve(_clients.size());
        for (const auto& client : _clients)
        {
            result.emplace_back(client.ClientInstanceId);
        }
        return result;
    }

    size_t RuntimeTerminalClientRegistry::Size() const noexcept
    {
        return _clients.size();
    }

    RuntimeTerminalClientRegistry::Client* RuntimeTerminalClientRegistry::_find(
        const std::string_view clientInstanceId) noexcept
    {
        const auto found = std::ranges::find_if(_clients, [&](const auto& client) {
            return client.ClientInstanceId == clientInstanceId;
        });
        return found == _clients.end() ? nullptr : &*found;
    }

    const RuntimeTerminalClientRegistry::Client* RuntimeTerminalClientRegistry::_find(
        const std::string_view clientInstanceId) const noexcept
    {
        const auto found = std::ranges::find_if(_clients, [&](const auto& client) {
            return client.ClientInstanceId == clientInstanceId;
        });
        return found == _clients.end() ? nullptr : &*found;
    }

    bool RuntimeTerminalClientRegistry::_validFrame(const TerminalFrame& frame) const
    {
        std::string error;
        return (frame.Generation == 0 ||
                frame.Generation == _configuration.Generation) &&
               ValidateTerminalFrame(
                   frame,
                   _configuration.MaximumFrameBytes,
                   error);
    }

    size_t RuntimeTerminalClientRegistry::_wireBytes(
        const TerminalFrame& frame) noexcept
    {
        return TerminalFrameHeaderBytes + frame.Payload.size();
    }
}
