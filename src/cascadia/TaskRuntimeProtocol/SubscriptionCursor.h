// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolRpc.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    enum class SubscriptionCursorState
    {
        Empty,
        Synchronized,
        ResnapshotRequired,
    };

    enum class NotificationDisposition
    {
        Apply,
        IgnoreDuplicate,
        IgnoreUnknown,
        Resnapshot,
        Invalid,
    };

    class SubscriptionCursor final
    {
    public:
        [[nodiscard]] bool ApplySnapshot(
            std::string subscriptionId,
            uint64_t generation,
            uint64_t cursor);
        [[nodiscard]] NotificationDisposition Observe(const ControlNotification& notification);
        void Reset() noexcept;

        [[nodiscard]] SubscriptionCursorState State() const noexcept;
        [[nodiscard]] const std::string& SubscriptionId() const noexcept;
        [[nodiscard]] uint64_t Generation() const noexcept;
        [[nodiscard]] uint64_t Cursor() const noexcept;

    private:
        SubscriptionCursorState _state{ SubscriptionCursorState::Empty };
        std::string _subscriptionId;
        uint64_t _generation{};
        uint64_t _cursor{};
    };
}
