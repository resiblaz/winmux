// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlQueue.h"

#include "ProtocolVersion.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    struct RuntimeControlQueue::Impl
    {
        size_t MaximumMessages{};
        size_t MaximumBytes{};
        size_t OutstandingMessages{};
        size_t OutstandingBytes{};
        bool IsClosed{};
        std::deque<RuntimeQueuedControlPayload> Pending;
        mutable std::mutex Mutex;
        std::condition_variable Changed;

        [[nodiscard]] bool Valid() const noexcept
        {
            return MaximumMessages != 0 &&
                   MaximumMessages <= AbsoluteMaximumQueuedControlMessages &&
                   MaximumBytes >= MinimumControlFrameBytes + sizeof(uint32_t) &&
                   MaximumBytes <= AbsoluteMaximumQueuedControlBytes;
        }
    };

    RuntimeControlQueue::RuntimeControlQueue(
        const size_t maximumMessages,
        const size_t maximumBytes) :
        _impl{ std::make_unique<Impl>() }
    {
        _impl->MaximumMessages = maximumMessages;
        _impl->MaximumBytes = maximumBytes;
    }

    RuntimeControlQueue::~RuntimeControlQueue() = default;

    RuntimeControlQueueStatus RuntimeControlQueue::Enqueue(
        std::string payload,
        std::optional<std::string> completionRequestId)
    {
        const auto wireBytes = payload.size() + sizeof(uint32_t);
        std::scoped_lock lock{ _impl->Mutex };
        if (_impl->IsClosed)
        {
            return RuntimeControlQueueStatus::Closed;
        }
        if (!_impl->Valid() || payload.empty() || payload.size() > MaximumControlFrameBytes ||
            wireBytes < payload.size())
        {
            return RuntimeControlQueueStatus::InvalidMessage;
        }
        if (_impl->OutstandingMessages >= _impl->MaximumMessages ||
            _impl->OutstandingBytes > _impl->MaximumBytes ||
            wireBytes > _impl->MaximumBytes - _impl->OutstandingBytes)
        {
            return RuntimeControlQueueStatus::QueueLimitReached;
        }

        RuntimeQueuedControlPayload queued;
        queued.Payload = std::move(payload);
        queued.CompletionRequestId = std::move(completionRequestId);
        queued.WireBytes = wireBytes;
        _impl->Pending.emplace_back(std::move(queued));
        ++_impl->OutstandingMessages;
        _impl->OutstandingBytes += wireBytes;
        _impl->Changed.notify_one();
        return RuntimeControlQueueStatus::Accepted;
    }

    std::optional<RuntimeQueuedControlPayload> RuntimeControlQueue::WaitDequeue()
    {
        std::unique_lock lock{ _impl->Mutex };
        _impl->Changed.wait(lock, [&] {
            return _impl->IsClosed || !_impl->Pending.empty();
        });
        if (_impl->Pending.empty())
        {
            return std::nullopt;
        }
        auto payload = std::move(_impl->Pending.front());
        _impl->Pending.pop_front();
        return payload;
    }

    void RuntimeControlQueue::Complete(const RuntimeQueuedControlPayload& payload) noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        if (_impl->OutstandingMessages != 0)
        {
            --_impl->OutstandingMessages;
        }
        _impl->OutstandingBytes = payload.WireBytes <= _impl->OutstandingBytes ?
                                      _impl->OutstandingBytes - payload.WireBytes :
                                      0;
    }

    void RuntimeControlQueue::Close() noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        if (_impl->IsClosed)
        {
            return;
        }
        _impl->IsClosed = true;
        for (const auto& payload : _impl->Pending)
        {
            if (_impl->OutstandingMessages != 0)
            {
                --_impl->OutstandingMessages;
            }
            _impl->OutstandingBytes = payload.WireBytes <= _impl->OutstandingBytes ?
                                          _impl->OutstandingBytes - payload.WireBytes :
                                          0;
        }
        _impl->Pending.clear();
        _impl->Changed.notify_all();
    }

    bool RuntimeControlQueue::Valid() const noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        return _impl->Valid();
    }

    bool RuntimeControlQueue::Closed() const noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        return _impl->IsClosed;
    }

    size_t RuntimeControlQueue::OutstandingMessageCount() const noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        return _impl->OutstandingMessages;
    }

    size_t RuntimeControlQueue::OutstandingByteCount() const noexcept
    {
        std::scoped_lock lock{ _impl->Mutex };
        return _impl->OutstandingBytes;
    }
}
