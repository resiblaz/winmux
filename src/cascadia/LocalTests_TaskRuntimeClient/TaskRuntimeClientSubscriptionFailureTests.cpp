// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeClientTestSupport.h"

#include <atomic>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeClientLocalTests::TestSupport;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        RuntimeClientSubscriptionEvent PullKind(
            const RuntimeClientSubscriptionHandle& subscription,
            const RuntimeClientSubscriptionEventKind kind)
        {
            const auto value = PullSubscriptionEvent(subscription.Stream);
            VERIFY_IS_TRUE(value.has_value());
            VERIFY_IS_TRUE(value && value->Kind == kind);
            return value.value_or(RuntimeClientSubscriptionEvent{});
        }

        std::string LargeJson(const size_t payloadBytes)
        {
            return "{\"value\":\"" + std::string(payloadBytes, 'x') + "\"}";
        }

        void InstallSlotResponder(
            const std::shared_ptr<FakeRuntimeClientConnectionState>& connection,
            const size_t wireOffset,
            const uint64_t cursorOffset)
        {
            connection->SetAutoResponseHandler([wireOffset, cursorOffset](const ControlRequest& request) {
                const auto second = request.PayloadJson.find("\"slot\":2") != std::string::npos;
                const auto index = wireOffset + (second ? 1 : 0);
                return SnapshotResponse(
                    request,
                    CanonicalSubscriptionIds[index],
                    wireOffset + 1,
                    cursorOffset + (second ? 10 : 0));
            });
        }
    }

    class TaskRuntimeClientSubscriptionFailureTests
    {
        TEST_CLASS(TaskRuntimeClientSubscriptionFailureTests);

        TEST_METHOD(PermanentOversizedSnapshotFaultsAtAttemptLimit);
        TEST_METHOD(StopOnFinalSnapshotAttemptReportsClientStopped);
        TEST_METHOD(NegotiatedFrameLimitRejectsOnlyOversizedSubscription);
        TEST_METHOD(MultipleSubscriptionsResnapshotTogetherOnNewEpoch);
    };

    void TaskRuntimeClientSubscriptionFailureTests::PermanentOversizedSnapshotFaultsAtAttemptLimit()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 4);
        auto configuration = ValidConfiguration();
        configuration.MaximumSubscriptionAttempts = 2;
        const auto largeSnapshot = LargeJson(1024);
        auto first = std::make_shared<FakeRuntimeClientConnectionState>();
        auto second = std::make_shared<FakeRuntimeClientConnectionState>();
        auto afterFault = std::make_shared<FakeRuntimeClientConnectionState>();
        InstallSubscriptionResponder(first, CanonicalSubscriptionIds[0], 1, 1, largeSnapshot);
        InstallSubscriptionResponder(second, CanonicalSubscriptionIds[1], 2, 2, largeSnapshot);
        fixture.Connector->EnqueueSuccess(first, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(second, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(afterFault, ValidHelloResponse(), true);

        TaskRuntimeClient client{ configuration, fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *first));
        RuntimeClientSubscribeOptions options;
        options.MaximumEventBytes = RuntimeClientSubscriptionEventOverheadBytes + 128;
        options.MaximumQueuedBytes = options.MaximumEventBytes;
        const auto subscription = client.Subscribe(options);
        VERIFY_IS_TRUE(subscription.Accepted);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*first, 1));
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*second, 1));

        const auto fault = PullKind(subscription, RuntimeClientSubscriptionEventKind::Faulted);
        VERIFY_IS_TRUE(fault.TransportStatus == RuntimeTransportStatus::Disconnected);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_RETRY }, fault.SystemError);
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSubscriptionFailureTests::StopOnFinalSnapshotAttemptReportsClientStopped()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 4);
        auto configuration = ValidConfiguration();
        configuration.MaximumSubscriptionAttempts = 2;
        auto first = std::make_shared<FakeRuntimeClientConnectionState>();
        auto finalAttempt = std::make_shared<FakeRuntimeClientConnectionState>();
        fixture.Connector->EnqueueSuccess(first, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(finalAttempt, ValidHelloResponse());

        TaskRuntimeClient client{ configuration, fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *first));
        const auto subscription = client.Subscribe();
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*first, 1));
        first->QueueInboundFailure(
            RuntimeTransportStatus::Disconnected,
            "first snapshot disconnected",
            ERROR_BROKEN_PIPE);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*finalAttempt, 1));

        client.RequestStop();
        VERIFY_IS_TRUE(client.WaitForStopped());
        (void)PullKind(subscription, RuntimeClientSubscriptionEventKind::ClientStopped);
    }

    void TaskRuntimeClientSubscriptionFailureTests::NegotiatedFrameLimitRejectsOnlyOversizedSubscription()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 4);
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        connection->SetAutoResponseHandler([](const ControlRequest& request) {
            return EmptySuccessResponse(request);
        });
        auto hello = ValidHelloResponse();
        hello.MaximumControlFrameBytes = MinimumControlFrameBytes;
        fixture.Connector->EnqueueSuccess(connection, std::move(hello));

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *connection));
        RuntimeClientSubscribeOptions options;
        options.ParamsJson = LargeJson(MinimumControlFrameBytes);
        const auto subscription = client.Subscribe(std::move(options));
        VERIFY_IS_TRUE(subscription.Accepted);
        const auto fault = PullKind(subscription, RuntimeClientSubscriptionEventKind::Faulted);
        VERIFY_IS_TRUE(fault.TransportStatus == RuntimeTransportStatus::ProtocolRejected);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_BAD_LENGTH }, fault.SystemError);
        VERIFY_IS_TRUE(client.State() == TaskRuntimeClientState::Ready);

        const auto ping = client.Submit(ControlMethod::Ping);
        VERIFY_IS_TRUE(WaitReady(ping));
        VERIFY_IS_TRUE(ping.Completion.get().Outcome == RuntimeClientRequestOutcome::Succeeded);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*connection, 1));
        VERIFY_IS_TRUE(connection->OutboundRequests().front().Method == ControlMethod::Ping);
        VERIFY_IS_TRUE(StopAndWait(client));
    }

    void TaskRuntimeClientSubscriptionFailureTests::MultipleSubscriptionsResnapshotTogetherOnNewEpoch()
    {
        RuntimeClientDependenciesFixture fixture;
        fixture.EnqueueRequestIds(0, 8);
        auto firstEpoch = std::make_shared<FakeRuntimeClientConnectionState>();
        auto secondEpoch = std::make_shared<FakeRuntimeClientConnectionState>();
        InstallSlotResponder(firstEpoch, 0, 10);
        InstallSlotResponder(secondEpoch, 2, 30);
        fixture.Connector->EnqueueSuccess(firstEpoch, ValidHelloResponse());
        fixture.Connector->EnqueueSuccess(secondEpoch, ValidHelloResponse());

        TaskRuntimeClient client{ ValidConfiguration(), fixture.Dependencies() };
        VERIFY_IS_TRUE(client.Start());
        VERIFY_IS_TRUE(WaitReady(client, *firstEpoch));
        RuntimeClientSubscribeOptions firstOptions;
        firstOptions.ParamsJson = "{\"slot\":1}";
        RuntimeClientSubscribeOptions secondOptions;
        secondOptions.ParamsJson = "{\"slot\":2}";
        const auto first = client.Subscribe(std::move(firstOptions));
        const auto second = client.Subscribe(std::move(secondOptions));
        VERIFY_ARE_EQUAL(uint64_t{ 10 }, PullKind(first, RuntimeClientSubscriptionEventKind::Snapshot).Cursor);
        VERIFY_ARE_EQUAL(uint64_t{ 20 }, PullKind(second, RuntimeClientSubscriptionEventKind::Snapshot).Cursor);

        VERIFY_IS_TRUE(firstEpoch->QueueInboundNotification(SubscriptionNotification(
            CanonicalSubscriptionIds[0],
            1,
            12)));
        (void)PullKind(first, RuntimeClientSubscriptionEventKind::ResnapshotRequired);
        VERIFY_ARE_EQUAL(uint64_t{ 30 }, PullKind(first, RuntimeClientSubscriptionEventKind::Snapshot).Cursor);
        VERIFY_ARE_EQUAL(uint64_t{ 40 }, PullKind(second, RuntimeClientSubscriptionEventKind::Snapshot).Cursor);
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*firstEpoch, 2));
        VERIFY_IS_TRUE(WaitForOutboundRequestCount(*secondEpoch, 2));
        VERIFY_IS_TRUE(StopAndWait(client));
    }
}
