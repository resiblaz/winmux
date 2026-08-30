// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientSubscriptions.h"
#include "RuntimeClientSubscriptionsInternal.h"

#include "ProtocolValidation.h"

#include <type_traits>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        bool _validIdentity(
            const RuntimeClientLogicalSubscriptionId logicalId,
            const RuntimeClientSubscriptionRevision revision) noexcept
        {
            return logicalId != 0 && revision != 0;
        }
    }

    static_assert(std::is_nothrow_move_assignable_v<SubscriptionCursor>);

    RuntimeClientSubscriptionRegistry::RuntimeClientSubscriptionRegistry(
        const RuntimeClientSubscriptionConfiguration configuration) :
        _impl{ std::make_unique<Impl>(configuration) }
    {
    }

    RuntimeClientSubscriptionRegistry::~RuntimeClientSubscriptionRegistry() = default;

    bool RuntimeClientSubscriptionRegistry::Valid() const noexcept
    {
        return _impl && _impl->IsValid;
    }

    RuntimeClientSubscriptionStatus RuntimeClientSubscriptionRegistry::Add(
        const RuntimeClientLogicalSubscriptionId logicalId,
        const RuntimeClientSubscriptionRevision revision)
    {
        if (!Valid() || !_validIdentity(logicalId, revision))
        {
            return RuntimeClientSubscriptionStatus::Invalid;
        }
        if (_impl->Records.contains(logicalId))
        {
            return RuntimeClientSubscriptionStatus::AlreadyExists;
        }
        if (_impl->Records.size() >= _impl->Configuration.MaximumSubscriptions)
        {
            return RuntimeClientSubscriptionStatus::CapacityExceeded;
        }
        try
        {
            RuntimeClientSubscriptionRecord record;
            record.Revision = revision;
            const auto inserted = _impl->Records.emplace(logicalId, std::move(record)).second;
            return inserted ? RuntimeClientSubscriptionStatus::Succeeded :
                              RuntimeClientSubscriptionStatus::AlreadyExists;
        }
        catch (...)
        {
            return RuntimeClientSubscriptionStatus::AllocationFailure;
        }
    }

    std::optional<RuntimeClientSubscriptionTarget>
        RuntimeClientSubscriptionRegistry::NextNeedingSnapshot() const noexcept
    {
        std::optional<RuntimeClientSubscriptionTarget> selected;
        if (!Valid())
        {
            return selected;
        }
        for (const auto& [logicalId, record] : _impl->Records)
        {
            if ((record.Phase == RuntimeClientSubscriptionPhase::Desired ||
                 record.Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot) &&
                (!selected || logicalId < selected->LogicalId))
            {
                selected = RuntimeClientSubscriptionTarget{ logicalId, record.Revision };
            }
        }
        return selected;
    }

    RuntimeClientSubscriptionStatus RuntimeClientSubscriptionRegistry::BeginSnapshot(
        const RuntimeClientLogicalSubscriptionId logicalId,
        const RuntimeClientSubscriptionRevision revision,
        const RuntimeClientEpoch epoch,
        const std::string_view requestId)
    {
        if (!Valid() || !_validIdentity(logicalId, revision) || epoch == 0 ||
            !IsCanonicalProtocolUuid(requestId))
        {
            return RuntimeClientSubscriptionStatus::Invalid;
        }
        const auto found = _impl->Records.find(logicalId);
        if (found == _impl->Records.end())
        {
            return RuntimeClientSubscriptionStatus::NotFound;
        }
        auto& record = found->second;
        if (record.Revision != revision)
        {
            return RuntimeClientSubscriptionStatus::Stale;
        }
        if (record.Phase != RuntimeClientSubscriptionPhase::Desired &&
            record.Phase != RuntimeClientSubscriptionPhase::NeedsSnapshot)
        {
            return RuntimeClientSubscriptionStatus::WrongPhase;
        }
        try
        {
            std::string recordRequestId{ requestId };
            const auto inserted = _impl->RequestBindings.emplace(
                recordRequestId,
                RuntimeClientSubscriptionRequestRecord{
                    logicalId,
                    revision,
                    epoch,
                    RuntimeClientSubscriptionRequestKind::Snapshot,
                });
            if (!inserted.second)
            {
                return RuntimeClientSubscriptionStatus::DuplicateRequestId;
            }
            record.RequestId = std::move(recordRequestId);
            record.Epoch = epoch;
            record.Phase = RuntimeClientSubscriptionPhase::SnapshotInFlight;
            return RuntimeClientSubscriptionStatus::Succeeded;
        }
        catch (...)
        {
            return RuntimeClientSubscriptionStatus::AllocationFailure;
        }
    }

    RuntimeClientSnapshotApplyResult RuntimeClientSubscriptionRegistry::ApplySnapshot(
        const RuntimeClientEpoch epoch,
        const std::string_view requestId,
        const SnapshotAndSubscribeResult& snapshot)
    {
        RuntimeClientSnapshotApplyResult result;
        if (!Valid() || epoch == 0 || !IsCanonicalProtocolUuid(requestId))
        {
            return result;
        }

        std::string key;
        try
        {
            key.assign(requestId);
        }
        catch (...)
        {
            result.Status = RuntimeClientSubscriptionStatus::AllocationFailure;
            return result;
        }
        const auto request = _impl->RequestBindings.find(key);
        if (request == _impl->RequestBindings.end() ||
            request->second.Kind != RuntimeClientSubscriptionRequestKind::Snapshot ||
            request->second.Epoch != epoch)
        {
            result.Status = RuntimeClientSubscriptionStatus::Stale;
            return result;
        }
        const auto found = _impl->Records.find(request->second.LogicalId);
        if (found == _impl->Records.end())
        {
            result.Status = RuntimeClientSubscriptionStatus::Stale;
            return result;
        }
        auto& record = found->second;
        if (record.Revision != request->second.Revision || record.Epoch != epoch ||
            record.RequestId != key ||
            (record.Phase != RuntimeClientSubscriptionPhase::SnapshotInFlight &&
             record.Phase != RuntimeClientSubscriptionPhase::Removing))
        {
            result.Status = RuntimeClientSubscriptionStatus::Stale;
            return result;
        }

        SubscriptionCursor staged;
        try
        {
            if (!staged.ApplySnapshot(snapshot.SubscriptionId, snapshot.Generation, snapshot.Cursor))
            {
                result.Status = RuntimeClientSubscriptionStatus::Invalid;
                return result;
            }
            const auto inserted = _impl->WireOwners.emplace(snapshot.SubscriptionId, request->second.LogicalId);
            if (!inserted.second)
            {
                result.Status = RuntimeClientSubscriptionStatus::WireConflict;
                return result;
            }
        }
        catch (...)
        {
            result.Status = RuntimeClientSubscriptionStatus::AllocationFailure;
            return result;
        }

        const auto wasRemoving = record.Phase == RuntimeClientSubscriptionPhase::Removing;
        record.Cursor = std::move(staged);
        record.RequestId.clear();
        _impl->RequestBindings.erase(request);
        record.Phase = wasRemoving ? RuntimeClientSubscriptionPhase::Removing :
                                     RuntimeClientSubscriptionPhase::Live;
        result.Status = RuntimeClientSubscriptionStatus::Succeeded;
        result.LogicalId = found->first;
        result.Revision = record.Revision;
        result.Phase = record.Phase;
        result.NeedsUnsubscribe = wasRemoving;
        return result;
    }

    std::optional<RuntimeClientSubscriptionRequestBinding>
        RuntimeClientSubscriptionRegistry::FindRequestBinding(
            const RuntimeClientEpoch epoch,
            const std::string_view requestId) const
    {
        if (!Valid() || epoch == 0 || !IsCanonicalProtocolUuid(requestId))
        {
            return std::nullopt;
        }

        std::string key;
        try
        {
            key.assign(requestId);
        }
        catch (...)
        {
            return std::nullopt;
        }
        const auto request = _impl->RequestBindings.find(key);
        if (request == _impl->RequestBindings.end() || request->second.Epoch != epoch)
        {
            return std::nullopt;
        }
        const auto found = _impl->Records.find(request->second.LogicalId);
        if (found == _impl->Records.end())
        {
            return std::nullopt;
        }
        const auto& record = found->second;
        if (record.Revision != request->second.Revision || record.Epoch != epoch ||
            record.RequestId != key)
        {
            return std::nullopt;
        }

        const auto snapshot = request->second.Kind == RuntimeClientSubscriptionRequestKind::Snapshot &&
                              (record.Phase == RuntimeClientSubscriptionPhase::SnapshotInFlight ||
                               record.Phase == RuntimeClientSubscriptionPhase::Removing) &&
                              record.Cursor.State() == SubscriptionCursorState::Empty;
        const auto unsubscribe = request->second.Kind == RuntimeClientSubscriptionRequestKind::Unsubscribe &&
                                 record.Phase == RuntimeClientSubscriptionPhase::UnsubscribeInFlight &&
                                 record.Cursor.State() == SubscriptionCursorState::Synchronized;
        if (!snapshot && !unsubscribe)
        {
            return std::nullopt;
        }
        return RuntimeClientSubscriptionRequestBinding{
            request->second.LogicalId,
            request->second.Revision,
            request->second.Kind,
        };
    }
}
