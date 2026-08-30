// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeControlQueue.h"

#include "ProtocolVersion.h"

#include <atomic>
#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeLocalTests
{
    class RuntimeControlQueueTests
    {
        TEST_CLASS(RuntimeControlQueueTests);

        TEST_METHOD(DequeuedPayloadCountsUntilWriterCompletesIt);
        TEST_METHOD(EnforcesByteAndMessageValidationBounds);
        TEST_METHOD(CloseWakesAWaitingWriter);
    };

    void RuntimeControlQueueTests::DequeuedPayloadCountsUntilWriterCompletesIt()
    {
        RuntimeControlQueue queue{ 2, 1024 };
        VERIFY_IS_TRUE(queue.Valid());
        VERIFY_IS_TRUE(queue.Enqueue("{}") == RuntimeControlQueueStatus::Accepted);
        VERIFY_IS_TRUE(queue.Enqueue("{\"second\":true}") == RuntimeControlQueueStatus::Accepted);
        VERIFY_IS_TRUE(queue.Enqueue("{\"third\":true}") == RuntimeControlQueueStatus::QueueLimitReached);

        const auto first = queue.WaitDequeue();
        VERIFY_IS_TRUE(first.has_value());
        VERIFY_ARE_EQUAL(size_t{ 2 }, queue.OutstandingMessageCount());
        VERIFY_IS_TRUE(queue.Enqueue("{\"third\":true}") == RuntimeControlQueueStatus::QueueLimitReached);

        queue.Complete(*first);
        VERIFY_ARE_EQUAL(size_t{ 1 }, queue.OutstandingMessageCount());
        VERIFY_IS_TRUE(queue.Enqueue("{\"third\":true}") == RuntimeControlQueueStatus::Accepted);
        queue.Close();
        VERIFY_IS_TRUE(queue.Closed());
        VERIFY_ARE_EQUAL(size_t{ 0 }, queue.OutstandingMessageCount());
        VERIFY_ARE_EQUAL(size_t{ 0 }, queue.OutstandingByteCount());
    }

    void RuntimeControlQueueTests::EnforcesByteAndMessageValidationBounds()
    {
        RuntimeControlQueue invalidCount{ 0, 1024 };
        VERIFY_IS_FALSE(invalidCount.Valid());
        VERIFY_IS_TRUE(invalidCount.Enqueue("{}") == RuntimeControlQueueStatus::InvalidMessage);

        RuntimeControlQueue queue{ 2, MinimumControlFrameBytes + sizeof(uint32_t) };
        VERIFY_IS_TRUE(queue.Valid());
        VERIFY_IS_TRUE(queue.Enqueue("") == RuntimeControlQueueStatus::InvalidMessage);
        VERIFY_IS_TRUE(queue.Enqueue(std::string(MinimumControlFrameBytes, 'x')) == RuntimeControlQueueStatus::Accepted);
        VERIFY_IS_TRUE(queue.Enqueue("{}") == RuntimeControlQueueStatus::QueueLimitReached);
        queue.Close();
        VERIFY_IS_TRUE(queue.Enqueue("{}") == RuntimeControlQueueStatus::Closed);
    }

    void RuntimeControlQueueTests::CloseWakesAWaitingWriter()
    {
        RuntimeControlQueue queue{ 2, 1024 };
        std::atomic_bool waiting{};
        std::optional<RuntimeQueuedControlPayload> result;
        std::thread writer{ [&] {
            waiting.store(true, std::memory_order_release);
            result = queue.WaitDequeue();
        } };
        while (!waiting.load(std::memory_order_acquire))
        {
            SwitchToThread();
        }
        queue.Close();
        writer.join();
        VERIFY_IS_FALSE(result.has_value());
    }
}
