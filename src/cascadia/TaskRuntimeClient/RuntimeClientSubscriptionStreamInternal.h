// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientSubscriptionStream.h"

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
        HANDLE Changed{};

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
            RuntimeClientSubscriptionRemoveCallback removeCallback);
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
            RuntimeClientSubscriptionRemoveCallback removeCallback = {});
    };
}
