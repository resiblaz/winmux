// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientSubscriptionStream.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>

namespace Microsoft::Terminal::TaskRuntime::details
{
    enum class RuntimeClientSubscriptionPublishStatus
    {
        Accepted,
        InvalidEvent,
        EventTooLarge,
        CountLimitReached,
        ByteLimitReached,
        AggregateLimitReached,
        Closed,
        AllocationFailure,
    };

    enum class RuntimeClientSubscriptionStreamCreateStatus
    {
        Succeeded,
        InvalidOptions,
        SystemError,
        AllocationFailure,
    };

    using RuntimeClientSubscriptionRemoveCallback = std::function<void(
        RuntimeClientLogicalSubscriptionId,
        RuntimeClientSubscriptionRevision)>;

    [[nodiscard]] bool NormalizeRuntimeClientSubscribeOptions(
        RuntimeClientSubscribeOptions& options) noexcept;
    [[nodiscard]] bool IsTerminalRuntimeClientSubscriptionEvent(
        RuntimeClientSubscriptionEventKind kind) noexcept;
    [[nodiscard]] bool ValidateRuntimeClientSubscriptionEvent(
        const RuntimeClientSubscriptionEvent& event,
        RuntimeClientLogicalSubscriptionId logicalId,
        RuntimeClientSubscriptionRevision revision) noexcept;
    [[nodiscard]] bool CanonicalizeRuntimeClientSubscriptionEvent(
        RuntimeClientSubscriptionEvent& event) noexcept;
    [[nodiscard]] std::optional<size_t> MeasureRuntimeClientSubscriptionEventBytes(
        const RuntimeClientSubscriptionEvent& event) noexcept;

    // Shared by every stream owned by one client. Reservation is atomic, so
    // stream code has one lock order: stream mutex, then lock-free budget.
    class RuntimeClientSubscriptionAggregateBudget final
    {
    public:
        explicit RuntimeClientSubscriptionAggregateBudget(
            size_t maximumBytes = DefaultMaximumClientSubscriptionAggregateBytes) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] bool TryReserve(size_t bytes) noexcept;
        void Release(size_t bytes) noexcept;
        [[nodiscard]] size_t UsedBytes() const noexcept;
        [[nodiscard]] size_t MaximumBytes() const noexcept;

    private:
        size_t _maximumBytes{};
        std::atomic_size_t _usedBytes{};
    };

    struct RuntimeClientQueuedSubscriptionEvent
    {
        RuntimeClientSubscriptionEvent Value;
        size_t Bytes{};
    };

    struct RuntimeClientSubscriptionStreamState final
    {
        RuntimeClientLogicalSubscriptionId LogicalId{};
        RuntimeClientSubscriptionRevision Revision{};
        size_t MaximumQueuedEvents{};
        size_t MaximumQueuedBytes{};
        size_t MaximumEventBytes{};
        std::shared_ptr<RuntimeClientSubscriptionAggregateBudget> AggregateBudget;
        HANDLE Changed{};
        HANDLE PullWaitEnteredEvent{}; // Non-owning deterministic test seam.

        mutable std::mutex Mutex;
        std::deque<RuntimeClientQueuedSubscriptionEvent> Queue;
        size_t QueuedBytes{};
        bool TerminalSet{};
        bool TerminalDelivered{};
        std::optional<RuntimeClientSubscriptionEvent> Terminal;
        bool RemovalArmed{};
        bool RemovalRequested{};
        RuntimeClientSubscriptionRemoveCallback RemoveCallback;

        RuntimeClientSubscriptionStreamState(
            RuntimeClientLogicalSubscriptionId logicalId,
            RuntimeClientSubscriptionRevision revision,
            size_t maximumQueuedEvents,
            size_t maximumQueuedBytes,
            size_t maximumEventBytes,
            std::shared_ptr<RuntimeClientSubscriptionAggregateBudget> aggregateBudget,
            RuntimeClientSubscriptionRemoveCallback removeCallback,
            HANDLE pullWaitEnteredEvent);
        ~RuntimeClientSubscriptionStreamState();

        RuntimeClientSubscriptionStreamState(const RuntimeClientSubscriptionStreamState&) = delete;
        RuntimeClientSubscriptionStreamState& operator=(const RuntimeClientSubscriptionStreamState&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeClientSubscriptionPublishStatus Publish(
            RuntimeClientSubscriptionEvent event) noexcept;
        [[nodiscard]] RuntimeClientSubscriptionPublishStatus Close(
            RuntimeClientSubscriptionEvent terminal) noexcept;
        [[nodiscard]] RuntimeClientSubscriptionPullResult Pull(
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent) noexcept;
        [[nodiscard]] bool RequestRemoval() noexcept;
    };

    class RuntimeClientSubscriptionStreamProducer final
    {
    public:
        explicit RuntimeClientSubscriptionStreamProducer(
            std::shared_ptr<RuntimeClientSubscriptionStreamState> state) noexcept;

        [[nodiscard]] RuntimeClientSubscriptionPublishStatus Publish(
            RuntimeClientSubscriptionEvent event) noexcept;
        [[nodiscard]] RuntimeClientSubscriptionPublishStatus Close(
            RuntimeClientSubscriptionEvent terminal) noexcept;

    private:
        std::shared_ptr<RuntimeClientSubscriptionStreamState> _state;
    };

    struct RuntimeClientSubscriptionStreamEndpoints
    {
        RuntimeClientSubscriptionStreamCreateStatus Status{
            RuntimeClientSubscriptionStreamCreateStatus::InvalidOptions
        };
        std::shared_ptr<RuntimeClientSubscriptionStream> Stream;
        std::shared_ptr<RuntimeClientSubscriptionStreamProducer> Producer;
    };

    struct RuntimeClientSubscriptionStreamFactory
    {
        [[nodiscard]] static RuntimeClientSubscriptionStreamEndpoints Create(
            RuntimeClientLogicalSubscriptionId logicalId,
            RuntimeClientSubscriptionRevision revision,
            RuntimeClientSubscribeOptions options,
            RuntimeClientSubscriptionRemoveCallback removeCallback = {},
            std::shared_ptr<RuntimeClientSubscriptionAggregateBudget> aggregateBudget = {},
            HANDLE pullWaitEnteredEvent = nullptr);
    };
}
