// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientSubscriptionStreamInternal.h"

#include "ProtocolRpc.h"
#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        bool _checkedAdd(size_t& total, const size_t value) noexcept
        {
            if (value > (std::numeric_limits<size_t>::max)() - total)
            {
                return false;
            }
            total += value;
            return true;
        }

        bool _compact(std::string& value) noexcept
        {
            try
            {
                std::string compact{ value.data(), value.size() };
                value.swap(compact);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool _validNotification(const ControlNotification& notification) noexcept
        {
            const auto knownKind = !ControlNotificationKindName(notification.Kind).empty();
            return IsCanonicalProtocolUuid(notification.SubscriptionId) &&
                   notification.Generation != 0 && notification.Sequence != 0 &&
                   !notification.PayloadJson.empty() &&
                   notification.PayloadJson.size() <= MaximumControlFrameBytes &&
                   IsValidUtf8(notification.PayloadJson) &&
                   notification.UnknownKind.size() <= MaximumNotificationKindBytes &&
                   IsValidUtf8(notification.UnknownKind) &&
                   (knownKind || !notification.UnknownKind.empty());
        }
    }

    bool NormalizeRuntimeClientSubscribeOptions(RuntimeClientSubscribeOptions& options) noexcept
    {
        options.MaximumQueuedEvents = options.MaximumQueuedEvents == 0 ?
                                          DefaultMaximumSubscriptionStreamEvents :
                                          options.MaximumQueuedEvents;
        options.MaximumQueuedBytes = options.MaximumQueuedBytes == 0 ?
                                         DefaultMaximumSubscriptionStreamBytes :
                                         options.MaximumQueuedBytes;
        options.MaximumEventBytes = options.MaximumEventBytes == 0 ?
                                        DefaultMaximumSubscriptionEventBytes :
                                        options.MaximumEventBytes;
        return options.MaximumQueuedEvents <= AbsoluteMaximumSubscriptionStreamEvents &&
               options.MaximumQueuedBytes >= RuntimeClientSubscriptionEventOverheadBytes &&
               options.MaximumQueuedBytes <= AbsoluteMaximumSubscriptionStreamBytes &&
               options.MaximumEventBytes >= RuntimeClientSubscriptionEventOverheadBytes &&
               options.MaximumEventBytes <= AbsoluteMaximumSubscriptionEventBytes &&
               options.MaximumEventBytes <= options.MaximumQueuedBytes &&
               !options.ParamsJson.empty() && options.ParamsJson.size() <= MaximumControlFrameBytes &&
               IsValidUtf8(options.ParamsJson);
    }

    bool IsTerminalRuntimeClientSubscriptionEvent(
        const RuntimeClientSubscriptionEventKind kind) noexcept
    {
        return kind == RuntimeClientSubscriptionEventKind::Removed ||
               kind == RuntimeClientSubscriptionEventKind::Faulted ||
               kind == RuntimeClientSubscriptionEventKind::ClientStopped;
    }

    bool ValidateRuntimeClientSubscriptionEvent(
        const RuntimeClientSubscriptionEvent& event,
        const RuntimeClientLogicalSubscriptionId logicalId,
        const RuntimeClientSubscriptionRevision revision) noexcept
    {
        if (event.LogicalId != logicalId || event.Revision != revision ||
            !IsValidUtf8(event.SnapshotJson) || !IsValidUtf8(event.Diagnostic) ||
            event.SnapshotJson.size() > MaximumControlFrameBytes ||
            event.Diagnostic.size() > MaximumSubscriptionEventDiagnosticBytes ||
            (event.Notification && !_validNotification(*event.Notification)))
        {
            return false;
        }
        switch (event.Kind)
        {
        case RuntimeClientSubscriptionEventKind::Snapshot:
            return event.Epoch != 0 && !event.SnapshotJson.empty() && !event.Notification;
        case RuntimeClientSubscriptionEventKind::Notification:
            return event.Epoch != 0 && event.SnapshotJson.empty() && event.Notification.has_value() &&
                   event.Cursor == event.Notification->Sequence;
        case RuntimeClientSubscriptionEventKind::ResnapshotRequired:
            return event.Epoch != 0 && event.SnapshotJson.empty();
        case RuntimeClientSubscriptionEventKind::Removed:
        case RuntimeClientSubscriptionEventKind::Faulted:
        case RuntimeClientSubscriptionEventKind::ClientStopped:
            return event.SnapshotJson.empty() && !event.Notification;
        }
        return false;
    }

    bool CanonicalizeRuntimeClientSubscriptionEvent(RuntimeClientSubscriptionEvent& event) noexcept
    {
        if (!_compact(event.SnapshotJson) || !_compact(event.Diagnostic))
        {
            return false;
        }
        return !event.Notification ||
               (_compact(event.Notification->SubscriptionId) &&
                _compact(event.Notification->UnknownKind) &&
                _compact(event.Notification->PayloadJson));
    }

    std::optional<size_t> MeasureRuntimeClientSubscriptionEventBytes(
        const RuntimeClientSubscriptionEvent& event) noexcept
    {
        size_t total{ RuntimeClientSubscriptionEventOverheadBytes };
        if ((!event.SnapshotJson.empty() && !_checkedAdd(total, event.SnapshotJson.capacity())) ||
            (!event.Diagnostic.empty() && !_checkedAdd(total, event.Diagnostic.capacity())))
        {
            return std::nullopt;
        }
        if (event.Notification &&
            ((!event.Notification->SubscriptionId.empty() &&
              !_checkedAdd(total, event.Notification->SubscriptionId.capacity())) ||
             (!event.Notification->UnknownKind.empty() &&
              !_checkedAdd(total, event.Notification->UnknownKind.capacity())) ||
             (!event.Notification->PayloadJson.empty() &&
              !_checkedAdd(total, event.Notification->PayloadJson.capacity()))))
        {
            return std::nullopt;
        }
        return total;
    }

    RuntimeClientSubscriptionAggregateBudget::RuntimeClientSubscriptionAggregateBudget(
        const size_t maximumBytes) noexcept :
        _maximumBytes{ maximumBytes }
    {
    }

    bool RuntimeClientSubscriptionAggregateBudget::Valid() const noexcept
    {
        return _maximumBytes != 0 &&
               _maximumBytes <= AbsoluteMaximumClientSubscriptionAggregateBytes;
    }

    bool RuntimeClientSubscriptionAggregateBudget::TryReserve(const size_t bytes) noexcept
    {
        if (!Valid() || bytes == 0 || bytes > _maximumBytes)
        {
            return false;
        }
        auto used = _usedBytes.load(std::memory_order_acquire);
        while (used <= _maximumBytes && bytes <= _maximumBytes - used)
        {
            if (_usedBytes.compare_exchange_weak(
                    used,
                    used + bytes,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return true;
            }
        }
        return false;
    }

    void RuntimeClientSubscriptionAggregateBudget::Release(const size_t bytes) noexcept
    {
        auto used = _usedBytes.load(std::memory_order_acquire);
        while (true)
        {
            const auto released = (std::min)(used, bytes);
            if (_usedBytes.compare_exchange_weak(
                    used,
                    used - released,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return;
            }
        }
    }

    size_t RuntimeClientSubscriptionAggregateBudget::UsedBytes() const noexcept
    {
        return _usedBytes.load(std::memory_order_acquire);
    }

    size_t RuntimeClientSubscriptionAggregateBudget::MaximumBytes() const noexcept
    {
        return _maximumBytes;
    }
}
