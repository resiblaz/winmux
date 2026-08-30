// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeClientSubscriptionStreamInternal.h"

#include "ProtocolVersion.h"

#include <atomic>
#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntime::details;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        constexpr RuntimeClientLogicalSubscriptionId LogicalId{ 17 };
        constexpr RuntimeClientSubscriptionRevision Revision{ 3 };
        constexpr RuntimeClientEpoch Epoch{ 5 };
        constexpr std::string_view WireId{ "42000000-0000-4000-8000-000000000001" };

        RuntimeClientSubscriptionEvent SnapshotEvent(
            const uint64_t cursor,
            std::string snapshotJson = "{}")
        {
            RuntimeClientSubscriptionEvent event;
            event.Kind = RuntimeClientSubscriptionEventKind::Snapshot;
            event.LogicalId = LogicalId;
            event.Revision = Revision;
            event.Epoch = Epoch;
            event.Cursor = cursor;
            event.SnapshotJson = std::move(snapshotJson);
            return event;
        }

        RuntimeClientSubscriptionEvent NotificationEvent(
            const uint64_t sequence,
            std::string payloadJson = "{}")
        {
            RuntimeClientSubscriptionEvent event;
            event.Kind = RuntimeClientSubscriptionEventKind::Notification;
            event.LogicalId = LogicalId;
            event.Revision = Revision;
            event.Epoch = Epoch;
            event.Cursor = sequence;
            ControlNotification notification;
            notification.SubscriptionId = WireId;
            notification.Generation = 2;
            notification.Sequence = sequence;
            notification.PayloadJson = std::move(payloadJson);
            event.Notification = std::move(notification);
            return event;
        }

        RuntimeClientSubscriptionEvent TerminalEvent(
            const RuntimeClientSubscriptionEventKind kind)
        {
            RuntimeClientSubscriptionEvent event;
            event.Kind = kind;
            event.LogicalId = LogicalId;
            event.Revision = Revision;
            event.Epoch = Epoch;
            return event;
        }

        RuntimeClientSubscriptionStreamEndpoints Create(
            RuntimeClientSubscribeOptions options = {},
            RuntimeClientSubscriptionRemoveCallback callback = {},
            std::shared_ptr<RuntimeClientSubscriptionAggregateBudget> aggregateBudget = {},
            const RuntimeClientLogicalSubscriptionId logicalId = LogicalId,
            const RuntimeClientSubscriptionRevision revision = Revision,
            const HANDLE pullWaitEnteredEvent = nullptr)
        {
            auto endpoints = RuntimeClientSubscriptionStreamFactory::Create(
                logicalId,
                revision,
                std::move(options),
                std::move(callback),
                std::move(aggregateBudget),
                pullWaitEnteredEvent);
            VERIFY_IS_TRUE(endpoints.Status == RuntimeClientSubscriptionStreamCreateStatus::Succeeded);
            VERIFY_IS_NOT_NULL(endpoints.Stream.get());
            VERIFY_IS_NOT_NULL(endpoints.Producer.get());
            return endpoints;
        }

        size_t CanonicalBytes(RuntimeClientSubscriptionEvent event)
        {
            VERIFY_IS_TRUE(CanonicalizeRuntimeClientSubscriptionEvent(event));
            const auto bytes = MeasureRuntimeClientSubscriptionEventBytes(event);
            VERIFY_IS_TRUE(bytes.has_value());
            return bytes.value_or(0);
        }
    }

    class RuntimeClientSubscriptionStreamTests
    {
        TEST_CLASS(RuntimeClientSubscriptionStreamTests);

        TEST_METHOD(DefaultsAreFrozenAndInvalidOptionsFailClosed);
        TEST_METHOD(PullsSnapshotAndNotificationInFifoOrder);
        TEST_METHOD(UnknownOptionalPayloadCountsTowardEventLimit);
        TEST_METHOD(ReservedCapacityCountsTowardRetainedMemory);
        TEST_METHOD(NotificationCursorMustMatchWireSequence);
        TEST_METHOD(EnforcesEventCountAndAggregateByteBounds);
        TEST_METHOD(PullReportsTimeoutCancellationAndClosed);
        TEST_METHOD(CloseWakesPullAtWaitBoundary);
        TEST_METHOD(TerminalEventSurvivesAFullQueueAndDrainsLast);
        TEST_METHOD(ConcurrentProducerAndConsumerPreserveOrder);
        TEST_METHOD(UnsubscribeAndDestructorInvokeRemoveExactlyOnce);
    };

    void RuntimeClientSubscriptionStreamTests::DefaultsAreFrozenAndInvalidOptionsFailClosed()
    {
        VERIFY_ARE_EQUAL(size_t{ 512 }, DefaultMaximumSubscriptionStreamEvents);
        VERIFY_ARE_EQUAL(size_t{ 4 * 1024 * 1024 }, DefaultMaximumSubscriptionStreamBytes);
        VERIFY_ARE_EQUAL(size_t{ 32 * 1024 * 1024 }, DefaultMaximumClientSubscriptionAggregateBytes);
        VERIFY_IS_TRUE(AbsoluteMaximumSubscriptionStreamEvents >= DefaultMaximumSubscriptionStreamEvents);
        auto defaults = Create();
        VERIFY_ARE_EQUAL(uint64_t{ LogicalId }, defaults.Stream->LogicalId());
        VERIFY_ARE_EQUAL(uint64_t{ Revision }, defaults.Stream->Revision());
        std::string maximumSnapshot(MaximumControlFrameBytes, ' ');
        maximumSnapshot.front() = '{';
        maximumSnapshot.back() = '}';
        VERIFY_IS_TRUE(defaults.Producer->Publish(SnapshotEvent(1, maximumSnapshot)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        (void)defaults.Stream->Pull(0);
        maximumSnapshot.push_back(' ');
        VERIFY_IS_TRUE(defaults.Producer->Publish(SnapshotEvent(2, std::move(maximumSnapshot))) ==
                       RuntimeClientSubscriptionPublishStatus::InvalidEvent);

        RuntimeClientSubscribeOptions invalid;
        invalid.MaximumQueuedEvents = AbsoluteMaximumSubscriptionStreamEvents + 1;
        VERIFY_IS_TRUE(RuntimeClientSubscriptionStreamFactory::Create(
                           LogicalId,
                           Revision,
                           invalid)
                           .Status == RuntimeClientSubscriptionStreamCreateStatus::InvalidOptions);
        invalid = {};
        invalid.MaximumEventBytes = AbsoluteMaximumSubscriptionEventBytes + 1;
        VERIFY_IS_TRUE(RuntimeClientSubscriptionStreamFactory::Create(
                           LogicalId,
                           Revision,
                           invalid)
                           .Status == RuntimeClientSubscriptionStreamCreateStatus::InvalidOptions);
        invalid = {};
        invalid.ParamsJson = std::string{ "\xc0\x80", 2 };
        VERIFY_IS_TRUE(RuntimeClientSubscriptionStreamFactory::Create(
                           LogicalId,
                           Revision,
                           invalid)
                           .Status == RuntimeClientSubscriptionStreamCreateStatus::InvalidOptions);
    }

    void RuntimeClientSubscriptionStreamTests::PullsSnapshotAndNotificationInFifoOrder()
    {
        auto endpoints = Create();
        auto snapshot = SnapshotEvent(10, "{\"tasks\":[]}");
        VERIFY_IS_TRUE(endpoints.Producer->Publish(snapshot) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        snapshot.SnapshotJson = "mutated-after-publish";
        VERIFY_IS_TRUE(endpoints.Producer->Publish(NotificationEvent(11, "{\"id\":1}")) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);

        const auto first = endpoints.Stream->Pull(0);
        VERIFY_IS_TRUE(first.Status == RuntimeClientSubscriptionPullStatus::Event);
        VERIFY_IS_TRUE(first.Value->Kind == RuntimeClientSubscriptionEventKind::Snapshot);
        VERIFY_ARE_EQUAL(std::string{ "{\"tasks\":[]}" }, first.Value->SnapshotJson);
        VERIFY_ARE_EQUAL(uint64_t{ 10 }, first.Value->Cursor);
        const auto second = endpoints.Stream->Pull(0);
        VERIFY_IS_TRUE(second.Value->Kind == RuntimeClientSubscriptionEventKind::Notification);
        VERIFY_ARE_EQUAL(uint64_t{ 11 }, second.Value->Notification->Sequence);
        VERIFY_ARE_EQUAL(std::string{ "{\"id\":1}" }, second.Value->Notification->PayloadJson);
    }

    void RuntimeClientSubscriptionStreamTests::UnknownOptionalPayloadCountsTowardEventLimit()
    {
        constexpr std::string_view unknownKind{ "future_optional_event" };
        constexpr size_t payloadBytes{ 64 };
        RuntimeClientSubscribeOptions options;
        options.MaximumQueuedEvents = 4;
        options.MaximumEventBytes = 512;
        options.MaximumQueuedBytes = options.MaximumEventBytes * 2;
        auto endpoints = Create(options);

        auto exact = NotificationEvent(1, std::string(payloadBytes, 'x'));
        exact.Notification->UnknownKind = unknownKind;
        VERIFY_IS_TRUE(endpoints.Producer->Publish(exact) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        auto oversized = NotificationEvent(2, std::string(1024, 'x'));
        oversized.Notification->UnknownKind = unknownKind;
        VERIFY_IS_TRUE(endpoints.Producer->Publish(std::move(oversized)) ==
                       RuntimeClientSubscriptionPublishStatus::EventTooLarge);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, endpoints.Stream->Pull(0).Value->Cursor);
    }

    void RuntimeClientSubscriptionStreamTests::EnforcesEventCountAndAggregateByteBounds()
    {
        RuntimeClientSubscribeOptions countOptions;
        countOptions.MaximumQueuedEvents = 1;
        countOptions.MaximumQueuedBytes = 1024;
        countOptions.MaximumEventBytes = 512;
        auto count = Create(countOptions);
        VERIFY_IS_TRUE(count.Producer->Publish(SnapshotEvent(1)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        VERIFY_IS_TRUE(count.Producer->Publish(SnapshotEvent(2)) ==
                       RuntimeClientSubscriptionPublishStatus::CountLimitReached);

        RuntimeClientSubscribeOptions byteOptions;
        byteOptions.MaximumQueuedEvents = 4;
        byteOptions.MaximumEventBytes = CanonicalBytes(SnapshotEvent(1));
        byteOptions.MaximumQueuedBytes = byteOptions.MaximumEventBytes * 2;
        auto bytes = Create(byteOptions);
        VERIFY_IS_TRUE(bytes.Producer->Publish(SnapshotEvent(1)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        VERIFY_IS_TRUE(bytes.Producer->Publish(SnapshotEvent(2)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        VERIFY_IS_TRUE(bytes.Producer->Publish(SnapshotEvent(3)) ==
                       RuntimeClientSubscriptionPublishStatus::ByteLimitReached);
        (void)bytes.Stream->Pull(0);
        VERIFY_IS_TRUE(bytes.Producer->Publish(SnapshotEvent(3)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
    }

    void RuntimeClientSubscriptionStreamTests::ReservedCapacityCountsTowardRetainedMemory()
    {
        RuntimeClientSubscribeOptions options;
        options.MaximumQueuedEvents = 2;
        options.MaximumQueuedBytes = 1024;
        options.MaximumEventBytes = 512;
        auto endpoints = Create(options);

        std::string oversizedCapacity;
        oversizedCapacity.reserve(4096);
        oversizedCapacity = "{}";
        VERIFY_IS_TRUE(endpoints.Producer->Publish(SnapshotEvent(1, std::move(oversizedCapacity))) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        const auto snapshot = endpoints.Stream->Pull(0);
        VERIFY_ARE_EQUAL(std::string{ "{}" }, snapshot.Value->SnapshotJson);
        VERIFY_IS_TRUE(snapshot.Value->SnapshotJson.capacity() <= options.MaximumEventBytes);

        auto terminal = TerminalEvent(RuntimeClientSubscriptionEventKind::Faulted);
        terminal.Diagnostic.reserve(4096);
        terminal.Diagnostic = "bounded diagnostic";
        VERIFY_IS_TRUE(endpoints.Producer->Close(std::move(terminal)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        const auto pulled = endpoints.Stream->Pull(0);
        VERIFY_IS_TRUE(pulled.Value->Kind == RuntimeClientSubscriptionEventKind::Faulted);
        VERIFY_ARE_EQUAL(std::string{ "bounded diagnostic" }, pulled.Value->Diagnostic);
        VERIFY_IS_TRUE(pulled.Value->Diagnostic.capacity() <= options.MaximumEventBytes);
    }

    void RuntimeClientSubscriptionStreamTests::NotificationCursorMustMatchWireSequence()
    {
        auto endpoints = Create();
        auto inconsistent = NotificationEvent(9);
        inconsistent.Cursor = 8;
        VERIFY_IS_TRUE(endpoints.Producer->Publish(std::move(inconsistent)) ==
                       RuntimeClientSubscriptionPublishStatus::InvalidEvent);
        VERIFY_IS_TRUE(endpoints.Stream->Pull(0).Status ==
                       RuntimeClientSubscriptionPullStatus::Timeout);
    }

    void RuntimeClientSubscriptionStreamTests::PullReportsTimeoutCancellationAndClosed()
    {
        auto endpoints = Create();
        VERIFY_IS_TRUE(endpoints.Stream->Pull(10).Status ==
                       RuntimeClientSubscriptionPullStatus::Timeout);
        const auto cancellation = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        VERIFY_IS_NOT_NULL(cancellation);
        VERIFY_IS_TRUE(endpoints.Stream->Pull(INFINITE, cancellation).Status ==
                       RuntimeClientSubscriptionPullStatus::Cancelled);
        CloseHandle(cancellation);

        VERIFY_IS_TRUE(endpoints.Producer->Close(
                           TerminalEvent(RuntimeClientSubscriptionEventKind::ClientStopped)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        const auto terminal = endpoints.Stream->Pull(0);
        VERIFY_IS_TRUE(terminal.Status == RuntimeClientSubscriptionPullStatus::Event);
        VERIFY_IS_TRUE(terminal.Value->Kind == RuntimeClientSubscriptionEventKind::ClientStopped);
        VERIFY_IS_TRUE(endpoints.Stream->Pull(0).Status == RuntimeClientSubscriptionPullStatus::Closed);
    }

    void RuntimeClientSubscriptionStreamTests::CloseWakesPullAtWaitBoundary()
    {
        const auto waitEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY_IS_NOT_NULL(waitEntered);
        auto endpoints = Create({}, {}, {}, LogicalId, Revision, waitEntered);
        RuntimeClientSubscriptionPullResult result;
        std::thread consumer{ [&] {
            result = endpoints.Stream->Pull(INFINITE);
        } };
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(waitEntered, 2000));
        VERIFY_IS_TRUE(endpoints.Producer->Close(
                           TerminalEvent(RuntimeClientSubscriptionEventKind::ClientStopped)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        consumer.join();
        VERIFY_IS_TRUE(result.Status == RuntimeClientSubscriptionPullStatus::Event);
        VERIFY_IS_TRUE(result.Value &&
                       result.Value->Kind == RuntimeClientSubscriptionEventKind::ClientStopped);
        CloseHandle(waitEntered);
    }

    void RuntimeClientSubscriptionStreamTests::TerminalEventSurvivesAFullQueueAndDrainsLast()
    {
        const auto eventBytes = CanonicalBytes(SnapshotEvent(1));
        RuntimeClientSubscribeOptions options;
        options.MaximumQueuedEvents = 1;
        options.MaximumQueuedBytes = eventBytes;
        options.MaximumEventBytes = eventBytes;
        auto endpoints = Create(options);
        VERIFY_IS_TRUE(endpoints.Producer->Publish(SnapshotEvent(1)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        VERIFY_IS_TRUE(endpoints.Producer->Publish(SnapshotEvent(2)) ==
                       RuntimeClientSubscriptionPublishStatus::CountLimitReached);
        auto fault = TerminalEvent(RuntimeClientSubscriptionEventKind::Faulted);
        fault.TransportStatus = RuntimeTransportStatus::Disconnected;
        fault.Diagnostic = "connection lost";
        VERIFY_IS_TRUE(endpoints.Producer->Close(std::move(fault)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        VERIFY_IS_TRUE(endpoints.Producer->Publish(SnapshotEvent(3)) ==
                       RuntimeClientSubscriptionPublishStatus::Closed);

        VERIFY_IS_TRUE(endpoints.Stream->Pull(0).Value->Kind ==
                       RuntimeClientSubscriptionEventKind::Snapshot);
        const auto terminal = endpoints.Stream->Pull(0);
        VERIFY_IS_TRUE(terminal.Value->Kind == RuntimeClientSubscriptionEventKind::Faulted);
        VERIFY_IS_TRUE(terminal.Value->TransportStatus == RuntimeTransportStatus::Disconnected);
        VERIFY_IS_TRUE(endpoints.Stream->Pull(0).Status == RuntimeClientSubscriptionPullStatus::Closed);

        auto fallback = Create();
        auto oversized = TerminalEvent(RuntimeClientSubscriptionEventKind::Faulted);
        oversized.SnapshotJson = "must be discarded";
        oversized.Notification = NotificationEvent(99).Notification;
        oversized.Diagnostic.assign(MaximumSubscriptionEventDiagnosticBytes + 1, 'x');
        VERIFY_IS_TRUE(fallback.Producer->Close(std::move(oversized)) ==
                       RuntimeClientSubscriptionPublishStatus::Accepted);
        const auto minimal = fallback.Stream->Pull(0);
        VERIFY_IS_TRUE(minimal.Value->Kind == RuntimeClientSubscriptionEventKind::Faulted);
        VERIFY_IS_TRUE(minimal.Value->SnapshotJson.empty());
        VERIFY_IS_FALSE(minimal.Value->Notification.has_value());
        VERIFY_IS_TRUE(minimal.Value->Diagnostic.empty());
    }

    void RuntimeClientSubscriptionStreamTests::ConcurrentProducerAndConsumerPreserveOrder()
    {
        RuntimeClientSubscribeOptions options;
        options.MaximumQueuedEvents = 128;
        options.MaximumQueuedBytes = 1024 * 1024;
        options.MaximumEventBytes = 1024;
        auto endpoints = Create(options);
        std::atomic_bool producerFailed{};
        std::thread producer{ [&] {
            for (uint64_t sequence = 1; sequence <= 64; ++sequence)
            {
                if (endpoints.Producer->Publish(NotificationEvent(sequence)) !=
                    RuntimeClientSubscriptionPublishStatus::Accepted)
                {
                    producerFailed.store(true, std::memory_order_release);
                    return;
                }
            }
        } };
        for (uint64_t sequence = 1; sequence <= 64; ++sequence)
        {
            const auto pulled = endpoints.Stream->Pull(2000);
            VERIFY_IS_TRUE(pulled.Status == RuntimeClientSubscriptionPullStatus::Event);
            VERIFY_ARE_EQUAL(sequence, pulled.Value->Cursor);
        }
        producer.join();
        VERIFY_IS_FALSE(producerFailed.load(std::memory_order_acquire));
    }

    void RuntimeClientSubscriptionStreamTests::UnsubscribeAndDestructorInvokeRemoveExactlyOnce()
    {
        std::atomic_uint32_t calls{};
        std::atomic_bool reentrantReturnedTrue{};
        std::weak_ptr<RuntimeClientSubscriptionStream> weak;
        auto endpoints = Create({}, [&](const auto logicalId, const auto revision) {
            VERIFY_ARE_EQUAL(uint64_t{ LogicalId }, logicalId);
            VERIFY_ARE_EQUAL(uint64_t{ Revision }, revision);
            calls.fetch_add(1, std::memory_order_relaxed);
            if (const auto stream = weak.lock())
            {
                reentrantReturnedTrue.store(stream->RequestUnsubscribe(), std::memory_order_release);
            }
        });
        weak = endpoints.Stream;
        VERIFY_IS_TRUE(endpoints.Stream->RequestUnsubscribe());
        VERIFY_IS_FALSE(endpoints.Stream->RequestUnsubscribe());
        endpoints.Stream.reset();
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, calls.load(std::memory_order_acquire));
        VERIFY_IS_FALSE(reentrantReturnedTrue.load(std::memory_order_acquire));

        auto destructorOnly = Create({}, [&](const auto, const auto) {
            calls.fetch_add(1, std::memory_order_relaxed);
        });
        destructorOnly.Stream.reset();
        VERIFY_ARE_EQUAL(uint32_t{ 2 }, calls.load(std::memory_order_acquire));
    }
}
