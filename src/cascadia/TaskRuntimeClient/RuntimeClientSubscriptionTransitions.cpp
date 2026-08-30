// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientSubscriptions.h"
#include "RuntimeClientSubscriptionsInternal.h"

#include "ProtocolValidation.h"

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

        bool _removing(const RuntimeClientSubscriptionPhase phase) noexcept
        {
            return phase == RuntimeClientSubscriptionPhase::Removing ||
                   phase == RuntimeClientSubscriptionPhase::UnsubscribeInFlight;
        }
    }

    RuntimeClientSubscriptionRemovalResult RuntimeClientSubscriptionRegistry::BeginRemove(
        const RuntimeClientLogicalSubscriptionId logicalId,
        const RuntimeClientSubscriptionRevision revision)
    {
        RuntimeClientSubscriptionRemovalResult result;
        if (!Valid() || !_validIdentity(logicalId, revision))
        {
            return result;
        }
        const auto found = _impl->Records.find(logicalId);
        if (found == _impl->Records.end())
        {
            result.Status = RuntimeClientSubscriptionStatus::NotFound;
            return result;
        }
        auto& record = found->second;
        if (record.Revision != revision)
        {
            result.Status = RuntimeClientSubscriptionStatus::Stale;
            return result;
        }
        if (record.Phase == RuntimeClientSubscriptionPhase::Desired ||
            record.Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot)
        {
            _impl->Records.erase(found);
            result.Status = RuntimeClientSubscriptionStatus::Finalized;
            result.Action = RuntimeClientSubscriptionRemovalAction::Finalized;
            return result;
        }
        if (record.Phase == RuntimeClientSubscriptionPhase::SnapshotInFlight)
        {
            record.Phase = RuntimeClientSubscriptionPhase::Removing;
            result.Status = RuntimeClientSubscriptionStatus::Succeeded;
            result.Action = RuntimeClientSubscriptionRemovalAction::AwaitSnapshot;
            return result;
        }
        if (record.Phase == RuntimeClientSubscriptionPhase::Removing &&
            record.Cursor.State() == SubscriptionCursorState::Empty &&
            !record.RequestId.empty())
        {
            result.Status = RuntimeClientSubscriptionStatus::Succeeded;
            result.Action = RuntimeClientSubscriptionRemovalAction::AwaitSnapshot;
            return result;
        }
        if (record.Phase == RuntimeClientSubscriptionPhase::UnsubscribeInFlight)
        {
            result.Status = RuntimeClientSubscriptionStatus::Succeeded;
            result.Action = RuntimeClientSubscriptionRemovalAction::UnsubscribeInFlight;
            return result;
        }
        if ((record.Phase != RuntimeClientSubscriptionPhase::Live &&
             record.Phase != RuntimeClientSubscriptionPhase::Removing) ||
            record.Cursor.State() != SubscriptionCursorState::Synchronized)
        {
            result.Status = RuntimeClientSubscriptionStatus::WrongPhase;
            return result;
        }
        try
        {
            UnsubscribeRequest request;
            request.SubscriptionId = record.Cursor.SubscriptionId();
            request.Generation = record.Cursor.Generation();
            result.Request = std::move(request);
        }
        catch (...)
        {
            result.Status = RuntimeClientSubscriptionStatus::AllocationFailure;
            return result;
        }
        record.Phase = RuntimeClientSubscriptionPhase::Removing;
        result.Status = RuntimeClientSubscriptionStatus::Succeeded;
        result.Action = RuntimeClientSubscriptionRemovalAction::SendUnsubscribe;
        return result;
    }

    RuntimeClientSubscriptionRemovalResult RuntimeClientSubscriptionRegistry::BeginRemoveForIntegration(
        const RuntimeClientLogicalSubscriptionId logicalId,
        const RuntimeClientSubscriptionRevision revision) noexcept
    {
        RuntimeClientSubscriptionRemovalResult result;
        if (!Valid() || !_validIdentity(logicalId, revision))
        {
            return result;
        }
        const auto found = _impl->Records.find(logicalId);
        if (found == _impl->Records.end())
        {
            result.Status = RuntimeClientSubscriptionStatus::NotFound;
            return result;
        }
        auto& record = found->second;
        if (record.Revision != revision)
        {
            result.Status = RuntimeClientSubscriptionStatus::Stale;
            return result;
        }
        if (record.Phase == RuntimeClientSubscriptionPhase::Desired ||
            record.Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot)
        {
            _impl->Records.erase(found);
            result.Status = RuntimeClientSubscriptionStatus::Finalized;
            result.Action = RuntimeClientSubscriptionRemovalAction::Finalized;
            return result;
        }
        if (record.Phase == RuntimeClientSubscriptionPhase::SnapshotInFlight)
        {
            record.Phase = RuntimeClientSubscriptionPhase::Removing;
            result.Status = RuntimeClientSubscriptionStatus::Succeeded;
            result.Action = RuntimeClientSubscriptionRemovalAction::AwaitSnapshot;
            return result;
        }
        if (record.Phase == RuntimeClientSubscriptionPhase::Removing &&
            record.Cursor.State() == SubscriptionCursorState::Empty &&
            !record.RequestId.empty())
        {
            result.Status = RuntimeClientSubscriptionStatus::Succeeded;
            result.Action = RuntimeClientSubscriptionRemovalAction::AwaitSnapshot;
            return result;
        }
        if (record.Phase == RuntimeClientSubscriptionPhase::UnsubscribeInFlight)
        {
            result.Status = RuntimeClientSubscriptionStatus::Succeeded;
            result.Action = RuntimeClientSubscriptionRemovalAction::UnsubscribeInFlight;
            return result;
        }
        if ((record.Phase != RuntimeClientSubscriptionPhase::Live &&
             record.Phase != RuntimeClientSubscriptionPhase::Removing) ||
            record.Cursor.State() != SubscriptionCursorState::Synchronized)
        {
            result.Status = RuntimeClientSubscriptionStatus::WrongPhase;
            return result;
        }
        record.Phase = RuntimeClientSubscriptionPhase::Removing;
        result.Status = RuntimeClientSubscriptionStatus::Succeeded;
        result.Action = RuntimeClientSubscriptionRemovalAction::SendUnsubscribe;
        return result;
    }

    RuntimeClientSubscriptionStatus RuntimeClientSubscriptionRegistry::BeginUnsubscribe(
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
        if (record.Revision != revision || record.Epoch != epoch)
        {
            return RuntimeClientSubscriptionStatus::Stale;
        }
        if (record.Phase != RuntimeClientSubscriptionPhase::Removing ||
            record.Cursor.State() != SubscriptionCursorState::Synchronized)
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
                    RuntimeClientSubscriptionRequestKind::Unsubscribe,
                });
            if (!inserted.second)
            {
                return RuntimeClientSubscriptionStatus::DuplicateRequestId;
            }
            record.RequestId = std::move(recordRequestId);
            record.Phase = RuntimeClientSubscriptionPhase::UnsubscribeInFlight;
            return RuntimeClientSubscriptionStatus::Succeeded;
        }
        catch (...)
        {
            return RuntimeClientSubscriptionStatus::AllocationFailure;
        }
    }

    RuntimeClientSubscriptionStatus RuntimeClientSubscriptionRegistry::CompleteUnsubscribe(
        const RuntimeClientEpoch epoch,
        const std::string_view requestId)
    {
        if (!Valid() || epoch == 0 || !IsCanonicalProtocolUuid(requestId))
        {
            return RuntimeClientSubscriptionStatus::Invalid;
        }
        std::string key;
        try
        {
            key.assign(requestId);
        }
        catch (...)
        {
            return RuntimeClientSubscriptionStatus::AllocationFailure;
        }
        const auto request = _impl->RequestBindings.find(key);
        if (request == _impl->RequestBindings.end() ||
            request->second.Kind != RuntimeClientSubscriptionRequestKind::Unsubscribe ||
            request->second.Epoch != epoch)
        {
            return RuntimeClientSubscriptionStatus::Stale;
        }
        const auto found = _impl->Records.find(request->second.LogicalId);
        if (found == _impl->Records.end() || found->second.Revision != request->second.Revision ||
            found->second.Phase != RuntimeClientSubscriptionPhase::UnsubscribeInFlight ||
            found->second.Epoch != epoch || found->second.RequestId != key)
        {
            return RuntimeClientSubscriptionStatus::Stale;
        }
        _impl->RemoveWire(found->second);
        _impl->RequestBindings.erase(request);
        _impl->Records.erase(found);
        return RuntimeClientSubscriptionStatus::Finalized;
    }

    RuntimeClientSubscriptionStatus RuntimeClientSubscriptionRegistry::FailRequest(
        const RuntimeClientEpoch epoch,
        const std::string_view requestId)
    {
        if (!Valid() || epoch == 0 || !IsCanonicalProtocolUuid(requestId))
        {
            return RuntimeClientSubscriptionStatus::Invalid;
        }
        std::string key;
        try
        {
            key.assign(requestId);
        }
        catch (...)
        {
            return RuntimeClientSubscriptionStatus::AllocationFailure;
        }
        const auto request = _impl->RequestBindings.find(key);
        if (request == _impl->RequestBindings.end() || request->second.Epoch != epoch)
        {
            return RuntimeClientSubscriptionStatus::Stale;
        }
        const auto found = _impl->Records.find(request->second.LogicalId);
        if (found == _impl->Records.end())
        {
            return RuntimeClientSubscriptionStatus::Stale;
        }
        auto& record = found->second;
        if (record.Revision != request->second.Revision || record.Epoch != epoch ||
            record.RequestId != key)
        {
            return RuntimeClientSubscriptionStatus::Stale;
        }

        if (request->second.Kind == RuntimeClientSubscriptionRequestKind::Snapshot)
        {
            if ((record.Phase != RuntimeClientSubscriptionPhase::SnapshotInFlight &&
                 record.Phase != RuntimeClientSubscriptionPhase::Removing) ||
                record.Cursor.State() != SubscriptionCursorState::Empty)
            {
                return RuntimeClientSubscriptionStatus::Stale;
            }
            const auto wasRemoving = record.Phase == RuntimeClientSubscriptionPhase::Removing;
            _impl->RequestBindings.erase(request);
            record.RequestId.clear();
            if (wasRemoving)
            {
                _impl->RemoveWire(record);
                _impl->Records.erase(found);
                return RuntimeClientSubscriptionStatus::Finalized;
            }
            _impl->RemoveWire(record);
            record.Epoch = 0;
            record.Phase = RuntimeClientSubscriptionPhase::NeedsSnapshot;
            return RuntimeClientSubscriptionStatus::Succeeded;
        }

        if (request->second.Kind != RuntimeClientSubscriptionRequestKind::Unsubscribe ||
            record.Phase != RuntimeClientSubscriptionPhase::UnsubscribeInFlight ||
            record.Cursor.State() != SubscriptionCursorState::Synchronized)
        {
            return RuntimeClientSubscriptionStatus::Stale;
        }
        _impl->RequestBindings.erase(request);
        record.RequestId.clear();
        record.Phase = RuntimeClientSubscriptionPhase::Removing;
        return RuntimeClientSubscriptionStatus::Succeeded;
    }

    RuntimeClientSubscriptionStatus RuntimeClientSubscriptionRegistry::ForceNeedsSnapshot(
        const RuntimeClientLogicalSubscriptionId logicalId,
        const RuntimeClientSubscriptionRevision revision,
        const RuntimeClientEpoch epoch) noexcept
    {
        if (!Valid() || !_validIdentity(logicalId, revision) || epoch == 0)
        {
            return RuntimeClientSubscriptionStatus::Invalid;
        }
        const auto found = _impl->Records.find(logicalId);
        if (found == _impl->Records.end())
        {
            return RuntimeClientSubscriptionStatus::NotFound;
        }
        auto& record = found->second;
        if (record.Revision != revision || record.Epoch != epoch)
        {
            return RuntimeClientSubscriptionStatus::Stale;
        }

        const auto wasRemoving = _removing(record.Phase);
        _impl->RemoveRequest(record);
        _impl->RemoveWire(record);
        if (wasRemoving)
        {
            _impl->Records.erase(found);
            return RuntimeClientSubscriptionStatus::Finalized;
        }
        record.Epoch = 0;
        record.Phase = RuntimeClientSubscriptionPhase::NeedsSnapshot;
        return RuntimeClientSubscriptionStatus::Succeeded;
    }

    RuntimeClientObservedNotification RuntimeClientSubscriptionRegistry::Observe(
        const RuntimeClientEpoch epoch,
        const ControlNotification& notification)
    {
        RuntimeClientObservedNotification result;
        if (!Valid() || epoch == 0)
        {
            return result;
        }
        const auto owner = _impl->WireOwners.find(notification.SubscriptionId);
        if (owner == _impl->WireOwners.end())
        {
            result.Status = RuntimeClientSubscriptionStatus::NotFound;
            return result;
        }
        const auto found = _impl->Records.find(owner->second);
        if (found == _impl->Records.end())
        {
            result.Status = RuntimeClientSubscriptionStatus::Stale;
            return result;
        }
        auto& record = found->second;
        result.LogicalId = found->first;
        result.Revision = record.Revision;
        result.Phase = record.Phase;
        if (record.Epoch != epoch)
        {
            result.Status = RuntimeClientSubscriptionStatus::Stale;
            return result;
        }
        if (record.Phase != RuntimeClientSubscriptionPhase::Live && !_removing(record.Phase))
        {
            result.Status = RuntimeClientSubscriptionStatus::WrongPhase;
            return result;
        }

        const auto wasRemoving = _removing(record.Phase);
        result.Disposition = record.Cursor.Observe(notification);
        result.Status = RuntimeClientSubscriptionStatus::Succeeded;
        result.Deliver = !wasRemoving && result.Disposition == NotificationDisposition::Apply;
        const auto closed = result.Disposition == NotificationDisposition::Apply &&
                            notification.Kind == ControlNotificationKind::SubscriptionClosed;
        if (result.Disposition == NotificationDisposition::Resnapshot || closed)
        {
            _impl->RemoveWire(record);
            if (wasRemoving)
            {
                _impl->RemoveRequest(record);
                _impl->Records.erase(found);
                result.Finalized = true;
            }
            else
            {
                _impl->RemoveRequest(record);
                record.Epoch = 0;
                record.Phase = RuntimeClientSubscriptionPhase::NeedsSnapshot;
                result.Phase = record.Phase;
            }
        }
        return result;
    }

    RuntimeClientSubscriptionRetireResult RuntimeClientSubscriptionRegistry::RetireEpoch(
        const RuntimeClientEpoch epoch) noexcept
    {
        RuntimeClientSubscriptionRetireResult result;
        if (!Valid() || epoch == 0)
        {
            return result;
        }
        result.Succeeded = true;
        auto current = _impl->Records.begin();
        while (current != _impl->Records.end())
        {
            auto& record = current->second;
            if (record.Epoch != epoch)
            {
                ++current;
                continue;
            }
            _impl->RemoveRequest(record);
            _impl->RemoveWire(record);
            if (_removing(record.Phase))
            {
                current = _impl->Records.erase(current);
                ++result.Finalized;
            }
            else
            {
                record.Epoch = 0;
                record.Phase = RuntimeClientSubscriptionPhase::NeedsSnapshot;
                ++result.NeedsSnapshot;
                ++current;
            }
        }
        return result;
    }

    std::optional<RuntimeClientSubscriptionView> RuntimeClientSubscriptionRegistry::Find(
        const RuntimeClientLogicalSubscriptionId logicalId) const
    {
        if (!Valid() || logicalId == 0)
        {
            return std::nullopt;
        }
        const auto found = _impl->Records.find(logicalId);
        if (found == _impl->Records.end())
        {
            return std::nullopt;
        }
        const auto& record = found->second;
        RuntimeClientSubscriptionView view;
        view.LogicalId = found->first;
        view.Revision = record.Revision;
        view.Phase = record.Phase;
        view.Epoch = record.Epoch;
        view.RequestId = record.RequestId;
        view.WireSubscriptionId = record.Cursor.SubscriptionId();
        view.Generation = record.Cursor.Generation();
        view.Cursor = record.Cursor.Cursor();
        return view;
    }

    size_t RuntimeClientSubscriptionRegistry::Size() const noexcept
    {
        return Valid() ? _impl->Records.size() : 0;
    }
}
