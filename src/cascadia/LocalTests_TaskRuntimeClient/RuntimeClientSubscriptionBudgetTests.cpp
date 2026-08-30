// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeClientSubscriptionStreamInternal.h"

#include <atomic>
#include <numeric>
#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntime::details;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        constexpr RuntimeClientLogicalSubscriptionId LogicalId{ 71 };
        constexpr RuntimeClientSubscriptionRevision Revision{ 4 };
        constexpr RuntimeClientEpoch Epoch{ 9 };

        RuntimeClientSubscriptionEvent SnapshotEvent(
            const uint64_t cursor,
            const RuntimeClientLogicalSubscriptionId logicalId = LogicalId)
        {
            RuntimeClientSubscriptionEvent event;
            event.Kind = RuntimeClientSubscriptionEventKind::Snapshot;
            event.LogicalId = logicalId;
            event.Revision = Revision;
            event.Epoch = Epoch;
            event.Cursor = cursor;
            event.SnapshotJson = "{}";
            return event;
        }

        RuntimeClientSubscriptionEvent TerminalEvent()
        {
            RuntimeClientSubscriptionEvent event;
            event.Kind = RuntimeClientSubscriptionEventKind::Faulted;
            event.LogicalId = LogicalId;
            event.Revision = Revision;
            event.Epoch = Epoch;
            return event;
        }

        size_t CanonicalBytes(RuntimeClientSubscriptionEvent event)
        {
            VERIFY_IS_TRUE(CanonicalizeRuntimeClientSubscriptionEvent(event));
            const auto bytes = MeasureRuntimeClientSubscriptionEventBytes(event);
            VERIFY_IS_TRUE(bytes.has_value());
            return bytes.value_or(0);
        }

        RuntimeClientSubscriptionStreamEndpoints Create(
            const RuntimeClientSubscribeOptions& options,
            const std::shared_ptr<RuntimeClientSubscriptionAggregateBudget>& budget,
            const RuntimeClientLogicalSubscriptionId logicalId = LogicalId)
        {
            auto endpoints = RuntimeClientSubscriptionStreamFactory::Create(
                logicalId,
                Revision,
                options,
                {},
                budget);
            VERIFY_IS_TRUE(endpoints.Status == RuntimeClientSubscriptionStreamCreateStatus::Succeeded);
            return endpoints;
        }
    }

    class RuntimeClientSubscriptionBudgetTests
    {
        TEST_CLASS(RuntimeClientSubscriptionBudgetTests);

        TEST_METHOD(SharedBudgetCoordinatesStreamsAndPullReleases);
        TEST_METHOD(DestroyingStateReleasesReservation);
        TEST_METHOD(ConcurrentReservationsNeverExceedLimit);
    };

    void RuntimeClientSubscriptionBudgetTests::SharedBudgetCoordinatesStreamsAndPullReleases()
    {
        const auto eventBytes = CanonicalBytes(SnapshotEvent(1));
        auto budget = std::make_shared<RuntimeClientSubscriptionAggregateBudget>(eventBytes);
        RuntimeClientSubscribeOptions options;
        options.MaximumQueuedEvents = 4;
        options.MaximumQueuedBytes = eventBytes * 4;
        options.MaximumEventBytes = eventBytes;
        auto first = Create(options, budget);
        auto second = Create(options, budget, LogicalId + 1);

        VERIFY_IS_TRUE(first.Producer->Publish(SnapshotEvent(1)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        auto secondEvent = SnapshotEvent(1, LogicalId + 1);
        VERIFY_IS_TRUE(second.Producer->Publish(secondEvent) ==
                       RuntimeClientSubscriptionPublishStatus::AggregateLimitReached);
        VERIFY_ARE_EQUAL(eventBytes, budget->UsedBytes());
        VERIFY_IS_TRUE(first.Stream->Pull(0).Value->Kind == RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_ARE_EQUAL(size_t{ 0 }, budget->UsedBytes());
        VERIFY_IS_TRUE(second.Producer->Publish(std::move(secondEvent)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);

        VERIFY_IS_TRUE(first.Producer->Close(TerminalEvent()) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        VERIFY_ARE_EQUAL(eventBytes, budget->UsedBytes());
        VERIFY_IS_TRUE(first.Stream->Pull(0).Value->Kind == RuntimeClientSubscriptionEventKind::Faulted);
        VERIFY_ARE_EQUAL(eventBytes, budget->UsedBytes());
        VERIFY_IS_TRUE(second.Stream->Pull(0).Value->Kind == RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_ARE_EQUAL(size_t{ 0 }, budget->UsedBytes());
    }

    void RuntimeClientSubscriptionBudgetTests::DestroyingStateReleasesReservation()
    {
        const auto eventBytes = CanonicalBytes(SnapshotEvent(1));
        auto budget = std::make_shared<RuntimeClientSubscriptionAggregateBudget>(eventBytes * 2);
        RuntimeClientSubscribeOptions options;
        options.MaximumQueuedEvents = 2;
        options.MaximumQueuedBytes = eventBytes * 2;
        options.MaximumEventBytes = eventBytes;
        auto endpoints = Create(options, budget);
        VERIFY_IS_TRUE(endpoints.Producer->Publish(SnapshotEvent(1)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        VERIFY_ARE_EQUAL(eventBytes, budget->UsedBytes());
        endpoints.Stream.reset();
        VERIFY_ARE_EQUAL(eventBytes, budget->UsedBytes());
        endpoints.Producer.reset();
        VERIFY_ARE_EQUAL(size_t{ 0 }, budget->UsedBytes());
    }

    void RuntimeClientSubscriptionBudgetTests::ConcurrentReservationsNeverExceedLimit()
    {
        constexpr size_t streamCount{ 8 };
        constexpr size_t attemptsPerStream{ 16 };
        constexpr size_t aggregateEvents{ 32 };
        const auto eventBytes = CanonicalBytes(SnapshotEvent(1));
        auto budget = std::make_shared<RuntimeClientSubscriptionAggregateBudget>(eventBytes * aggregateEvents);
        RuntimeClientSubscribeOptions options;
        options.MaximumQueuedEvents = attemptsPerStream;
        options.MaximumQueuedBytes = eventBytes * attemptsPerStream;
        options.MaximumEventBytes = eventBytes;
        std::vector<RuntimeClientSubscriptionStreamEndpoints> streams;
        streams.reserve(streamCount);
        for (size_t index = 0; index < streamCount; ++index)
        {
            streams.emplace_back(Create(options, budget, LogicalId + index));
        }

        std::array<size_t, streamCount> accepted{};
        std::atomic_size_t maximumObserved{};
        std::atomic_bool invalidStatus{};
        std::vector<std::thread> producers;
        producers.reserve(streamCount);
        for (size_t index = 0; index < streamCount; ++index)
        {
            producers.emplace_back([&, index] {
                for (size_t attempt = 0; attempt < attemptsPerStream; ++attempt)
                {
                    const auto status = streams[index].Producer->Publish(
                        SnapshotEvent(attempt + 1, LogicalId + index));
                    if (status == RuntimeClientSubscriptionPublishStatus::Accepted)
                    {
                        ++accepted[index];
                        auto observed = budget->UsedBytes();
                        auto maximum = maximumObserved.load(std::memory_order_acquire);
                        while (maximum < observed && !maximumObserved.compare_exchange_weak(
                                   maximum,
                                   observed,
                                   std::memory_order_acq_rel,
                                   std::memory_order_acquire))
                        {
                        }
                    }
                    else if (status != RuntimeClientSubscriptionPublishStatus::AggregateLimitReached)
                    {
                        invalidStatus.store(true, std::memory_order_release);
                    }
                }
            });
        }
        for (auto& producer : producers)
        {
            producer.join();
        }
        const auto acceptedTotal = std::accumulate(accepted.begin(), accepted.end(), size_t{});
        VERIFY_IS_FALSE(invalidStatus.load(std::memory_order_acquire));
        VERIFY_IS_TRUE(maximumObserved.load(std::memory_order_acquire) <= budget->MaximumBytes());
        VERIFY_ARE_EQUAL(acceptedTotal * eventBytes, budget->UsedBytes());

        for (size_t index = 0; index < streamCount; ++index)
        {
            for (size_t event = 0; event < accepted[index]; ++event)
            {
                VERIFY_IS_TRUE(streams[index].Stream->Pull(0).Status ==
                               RuntimeClientSubscriptionPullStatus::Event);
            }
        }
        VERIFY_ARE_EQUAL(size_t{ 0 }, budget->UsedBytes());
    }
}
