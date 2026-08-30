// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr size_t DefaultMaximumQueuedControlMessages{ 64 };
    inline constexpr size_t AbsoluteMaximumQueuedControlMessages{ 256 };
    inline constexpr size_t DefaultMaximumQueuedControlBytes{ 8 * 1024 * 1024 };
    inline constexpr size_t AbsoluteMaximumQueuedControlBytes{ 64 * 1024 * 1024 };

    enum class RuntimeControlQueueStatus
    {
        Accepted,
        Closed,
        InvalidMessage,
        QueueLimitReached,
    };

    struct RuntimeQueuedControlPayload
    {
        std::string Payload;
        std::optional<std::string> CompletionRequestId;
        size_t WireBytes{};
    };

    class RuntimeControlQueue final
    {
    public:
        RuntimeControlQueue(size_t maximumMessages, size_t maximumBytes);
        ~RuntimeControlQueue();

        RuntimeControlQueue(const RuntimeControlQueue&) = delete;
        RuntimeControlQueue& operator=(const RuntimeControlQueue&) = delete;

        [[nodiscard]] RuntimeControlQueueStatus Enqueue(
            std::string payload,
            std::optional<std::string> completionRequestId = std::nullopt);
        [[nodiscard]] std::optional<RuntimeQueuedControlPayload> WaitDequeue();
        void Complete(const RuntimeQueuedControlPayload& payload) noexcept;
        void Close() noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] bool Closed() const noexcept;
        [[nodiscard]] size_t OutstandingMessageCount() const noexcept;
        [[nodiscard]] size_t OutstandingByteCount() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
