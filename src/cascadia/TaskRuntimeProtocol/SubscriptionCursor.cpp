// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SubscriptionCursor.h"

#include "ProtocolValidation.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    bool SubscriptionCursor::ApplySnapshot(
        std::string subscriptionId,
        const uint64_t generation,
        const uint64_t cursor)
    {
        if (!IsCanonicalProtocolUuid(subscriptionId) || generation == 0)
        {
            return false;
        }
        _subscriptionId = std::move(subscriptionId);
        _generation = generation;
        _cursor = cursor;
        _state = SubscriptionCursorState::Synchronized;
        return true;
    }

    NotificationDisposition SubscriptionCursor::Observe(const ControlNotification& notification)
    {
        if (!IsCanonicalProtocolUuid(notification.SubscriptionId) ||
            notification.Generation == 0 || notification.Sequence == 0 ||
            notification.UnknownKind.size() > MaximumNotificationKindBytes ||
            !IsValidUtf8(notification.UnknownKind))
        {
            return NotificationDisposition::Invalid;
        }
        if (_state != SubscriptionCursorState::Synchronized ||
            notification.SubscriptionId != _subscriptionId ||
            notification.Generation != _generation)
        {
            _state = SubscriptionCursorState::ResnapshotRequired;
            return NotificationDisposition::Resnapshot;
        }
        if (notification.Sequence <= _cursor)
        {
            return NotificationDisposition::IgnoreDuplicate;
        }
        if (_cursor == std::numeric_limits<uint64_t>::max() ||
            notification.Sequence != _cursor + 1)
        {
            _state = SubscriptionCursorState::ResnapshotRequired;
            return NotificationDisposition::Resnapshot;
        }

        _cursor = notification.Sequence;
        if (!notification.UnknownKind.empty())
        {
            return NotificationDisposition::IgnoreUnknown;
        }
        if (notification.Kind == ControlNotificationKind::SubscriptionClosed)
        {
            _state = SubscriptionCursorState::ResnapshotRequired;
        }
        return NotificationDisposition::Apply;
    }

    void SubscriptionCursor::Reset() noexcept
    {
        _subscriptionId.clear();
        _generation = 0;
        _cursor = 0;
        _state = SubscriptionCursorState::Empty;
    }

    SubscriptionCursorState SubscriptionCursor::State() const noexcept
    {
        return _state;
    }

    const std::string& SubscriptionCursor::SubscriptionId() const noexcept
    {
        return _subscriptionId;
    }

    uint64_t SubscriptionCursor::Generation() const noexcept
    {
        return _generation;
    }

    uint64_t SubscriptionCursor::Cursor() const noexcept
    {
        return _cursor;
    }
}
