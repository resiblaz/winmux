// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientTypes.h"

#include "SnapshotAndSubscribe.h"
#include "SubscriptionCursor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    using RuntimeClientLogicalSubscriptionId = uint64_t;
    using RuntimeClientSubscriptionRevision = uint64_t;

    inline constexpr size_t DefaultMaximumRuntimeClientSubscriptions{ 64 };
    inline constexpr size_t AbsoluteMaximumRuntimeClientSubscriptions{ 256 };

    enum class RuntimeClientSubscriptionPhase
    {
        Desired,
        NeedsSnapshot,
        SnapshotInFlight,
        Live,
        Removing,
        UnsubscribeInFlight,
    };

    enum class RuntimeClientSubscriptionStatus
    {
        Succeeded,
        Finalized,
        Invalid,
        NotFound,
        Stale,
        WrongPhase,
        CapacityExceeded,
        AlreadyExists,
        DuplicateRequestId,
        WireConflict,
        AllocationFailure,
    };

    enum class RuntimeClientSubscriptionRemovalAction
    {
        None,
        Finalized,
        AwaitSnapshot,
        SendUnsubscribe,
        UnsubscribeInFlight,
    };

    struct RuntimeClientSubscriptionConfiguration
    {
        size_t MaximumSubscriptions{ DefaultMaximumRuntimeClientSubscriptions };
    };

    struct RuntimeClientSubscriptionTarget
    {
        RuntimeClientLogicalSubscriptionId LogicalId{};
        RuntimeClientSubscriptionRevision Revision{};
    };

    struct RuntimeClientSubscriptionView
    {
        RuntimeClientLogicalSubscriptionId LogicalId{};
        RuntimeClientSubscriptionRevision Revision{};
        RuntimeClientSubscriptionPhase Phase{ RuntimeClientSubscriptionPhase::Desired };
        RuntimeClientEpoch Epoch{};
        std::string RequestId;
        std::string WireSubscriptionId;
        uint64_t Generation{};
        uint64_t Cursor{};
    };

    struct RuntimeClientSubscriptionRemovalResult
    {
        RuntimeClientSubscriptionStatus Status{ RuntimeClientSubscriptionStatus::Invalid };
        RuntimeClientSubscriptionRemovalAction Action{ RuntimeClientSubscriptionRemovalAction::None };
        std::optional<Microsoft::Terminal::TaskRuntimeProtocol::UnsubscribeRequest> Request;
    };

    struct RuntimeClientSnapshotApplyResult
    {
        RuntimeClientSubscriptionStatus Status{ RuntimeClientSubscriptionStatus::Invalid };
        RuntimeClientLogicalSubscriptionId LogicalId{};
        RuntimeClientSubscriptionRevision Revision{};
        RuntimeClientSubscriptionPhase Phase{ RuntimeClientSubscriptionPhase::Desired };
        bool NeedsUnsubscribe{};
    };

    struct RuntimeClientObservedNotification
    {
        RuntimeClientSubscriptionStatus Status{ RuntimeClientSubscriptionStatus::Invalid };
        RuntimeClientLogicalSubscriptionId LogicalId{};
        RuntimeClientSubscriptionRevision Revision{};
        Microsoft::Terminal::TaskRuntimeProtocol::NotificationDisposition Disposition{
            Microsoft::Terminal::TaskRuntimeProtocol::NotificationDisposition::Invalid
        };
        RuntimeClientSubscriptionPhase Phase{ RuntimeClientSubscriptionPhase::Desired };
        bool Deliver{};
        bool Finalized{};
    };

    struct RuntimeClientSubscriptionRetireResult
    {
        bool Succeeded{};
        size_t NeedsSnapshot{};
        size_t Finalized{};
    };

    // Pure caller-serialized state. This type performs no IO, waiting, retry
    // creation, callback dispatch, or user-code invocation.
    class RuntimeClientSubscriptionRegistry final
    {
    public:
        explicit RuntimeClientSubscriptionRegistry(RuntimeClientSubscriptionConfiguration configuration);
        ~RuntimeClientSubscriptionRegistry();

        RuntimeClientSubscriptionRegistry(const RuntimeClientSubscriptionRegistry&) = delete;
        RuntimeClientSubscriptionRegistry& operator=(const RuntimeClientSubscriptionRegistry&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeClientSubscriptionStatus Add(
            RuntimeClientLogicalSubscriptionId logicalId,
            RuntimeClientSubscriptionRevision revision);
        [[nodiscard]] std::optional<RuntimeClientSubscriptionTarget> NextNeedingSnapshot() const noexcept;
        [[nodiscard]] RuntimeClientSubscriptionStatus BeginSnapshot(
            RuntimeClientLogicalSubscriptionId logicalId,
            RuntimeClientSubscriptionRevision revision,
            RuntimeClientEpoch epoch,
            std::string_view requestId);
        [[nodiscard]] RuntimeClientSnapshotApplyResult ApplySnapshot(
            RuntimeClientEpoch epoch,
            std::string_view requestId,
            const Microsoft::Terminal::TaskRuntimeProtocol::SnapshotAndSubscribeResult& snapshot);
        [[nodiscard]] RuntimeClientSubscriptionRemovalResult BeginRemove(
            RuntimeClientLogicalSubscriptionId logicalId,
            RuntimeClientSubscriptionRevision revision);
        [[nodiscard]] RuntimeClientSubscriptionStatus BeginUnsubscribe(
            RuntimeClientLogicalSubscriptionId logicalId,
            RuntimeClientSubscriptionRevision revision,
            RuntimeClientEpoch epoch,
            std::string_view requestId);
        [[nodiscard]] RuntimeClientSubscriptionStatus CompleteUnsubscribe(
            RuntimeClientEpoch epoch,
            std::string_view requestId);
        [[nodiscard]] RuntimeClientObservedNotification Observe(
            RuntimeClientEpoch epoch,
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlNotification& notification);
        [[nodiscard]] RuntimeClientSubscriptionRetireResult RetireEpoch(RuntimeClientEpoch epoch) noexcept;

        [[nodiscard]] std::optional<RuntimeClientSubscriptionView> Find(
            RuntimeClientLogicalSubscriptionId logicalId) const;
        [[nodiscard]] size_t Size() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
