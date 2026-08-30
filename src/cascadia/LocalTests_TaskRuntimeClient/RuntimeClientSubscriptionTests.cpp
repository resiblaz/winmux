// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeClientSubscriptions.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        constexpr std::array<std::string_view, 8> RequestIds{
            "31000000-0000-4000-8000-000000000001",
            "31000000-0000-4000-8000-000000000002",
            "31000000-0000-4000-8000-000000000003",
            "31000000-0000-4000-8000-000000000004",
            "31000000-0000-4000-8000-000000000005",
            "31000000-0000-4000-8000-000000000006",
            "31000000-0000-4000-8000-000000000007",
            "31000000-0000-4000-8000-000000000008",
        };
        constexpr std::array<std::string_view, 6> WireIds{
            "41000000-0000-4000-8000-000000000001",
            "41000000-0000-4000-8000-000000000002",
            "41000000-0000-4000-8000-000000000003",
            "41000000-0000-4000-8000-000000000004",
            "41000000-0000-4000-8000-000000000005",
            "41000000-0000-4000-8000-000000000006",
        };

        SnapshotAndSubscribeResult Snapshot(
            const std::string_view wireId,
            const uint64_t generation = 1,
            const uint64_t cursor = 10)
        {
            SnapshotAndSubscribeResult value;
            value.SubscriptionId = wireId;
            value.Generation = generation;
            value.Cursor = cursor;
            return value;
        }

        ControlNotification Notification(
            const std::string_view wireId,
            const uint64_t generation,
            const uint64_t sequence,
            const ControlNotificationKind kind = ControlNotificationKind::ProjectionChanged)
        {
            ControlNotification value;
            value.SubscriptionId = wireId;
            value.Generation = generation;
            value.Sequence = sequence;
            value.Kind = kind;
            return value;
        }

        void MakeLive(
            RuntimeClientSubscriptionRegistry& registry,
            const RuntimeClientLogicalSubscriptionId logicalId,
            const RuntimeClientSubscriptionRevision revision,
            const RuntimeClientEpoch epoch,
            const std::string_view requestId,
            const std::string_view wireId,
            const uint64_t generation = 1,
            const uint64_t cursor = 10)
        {
            VERIFY_IS_TRUE(registry.Add(logicalId, revision) == RuntimeClientSubscriptionStatus::Succeeded);
            VERIFY_IS_TRUE(registry.BeginSnapshot(logicalId, revision, epoch, requestId) ==
                           RuntimeClientSubscriptionStatus::Succeeded);
            VERIFY_IS_TRUE(registry.ApplySnapshot(epoch, requestId, Snapshot(wireId, generation, cursor)).Status ==
                           RuntimeClientSubscriptionStatus::Succeeded);
        }
    }

    class RuntimeClientSubscriptionTests
    {
        TEST_CLASS(RuntimeClientSubscriptionTests);

        TEST_METHOD(BoundsLogicalIdentityAndSelectsDesiredDeterministically);
        TEST_METHOD(BindsSnapshotToEpochRequestAndAppliesCursorAtomically);
        TEST_METHOD(ObservesKnownDuplicateAndUnknownNotifications);
        TEST_METHOD(GapGenerationDriftAndClosedRequireResnapshot);
        TEST_METHOD(RemovesLiveSubscriptionThroughBoundUnsubscribe);
        TEST_METHOD(RemoveDuringSnapshotWaitsThenCleansWireSubscription);
        TEST_METHOD(RemovedLogicalIdRejectsOldSnapshotAfterReuse);
        TEST_METHOD(RetireEpochResnapshotsDesiredAndFinalizesRemoving);
        TEST_METHOD(RejectsWireConflictDuplicateRequestAndStaleRevision);
        TEST_METHOD(StaleEpochAndUnknownWireDoNotMutateCursor);
        TEST_METHOD(FindsSnapshotAndUnsubscribeRequestBindings);
        TEST_METHOD(SnapshotRequestFailureRetriesOrFinalizesRemoval);
        TEST_METHOD(UnsubscribeRequestFailurePreservesWireForRetry);
        TEST_METHOD(ForceNeedsSnapshotRejectsStaleAndFinalizesRemoval);
    };

    void RuntimeClientSubscriptionTests::BoundsLogicalIdentityAndSelectsDesiredDeterministically()
    {
        RuntimeClientSubscriptionRegistry invalidZero{ { 0 } };
        RuntimeClientSubscriptionRegistry invalidLarge{ { 257 } };
        VERIFY_IS_FALSE(invalidZero.Valid());
        VERIFY_IS_FALSE(invalidLarge.Valid());

        RuntimeClientSubscriptionRegistry registry{ { 2 } };
        VERIFY_IS_TRUE(registry.Valid());
        VERIFY_IS_TRUE(registry.Add(0, 1) == RuntimeClientSubscriptionStatus::Invalid);
        VERIFY_IS_TRUE(registry.Add(2, 3) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.Add(1, 4) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.Add(1, 5) == RuntimeClientSubscriptionStatus::AlreadyExists);
        VERIFY_IS_TRUE(registry.Add(3, 1) == RuntimeClientSubscriptionStatus::CapacityExceeded);
        const auto next = registry.NextNeedingSnapshot();
        VERIFY_IS_TRUE(next.has_value());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, next->LogicalId);
        VERIFY_ARE_EQUAL(uint64_t{ 4 }, next->Revision);
        VERIFY_ARE_EQUAL(size_t{ 2 }, registry.Size());
    }

    void RuntimeClientSubscriptionTests::BindsSnapshotToEpochRequestAndAppliesCursorAtomically()
    {
        RuntimeClientSubscriptionRegistry registry{ { 2 } };
        VERIFY_IS_TRUE(registry.Add(1, 7) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 7, 2, "bad") == RuntimeClientSubscriptionStatus::Invalid);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 7, 2, RequestIds[0]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.ApplySnapshot(3, RequestIds[0], Snapshot(WireIds[0])).Status ==
                       RuntimeClientSubscriptionStatus::Stale);
        auto view = registry.Find(1);
        VERIFY_IS_TRUE(view->Phase == RuntimeClientSubscriptionPhase::SnapshotInFlight);
        VERIFY_ARE_EQUAL(std::string{ RequestIds[0] }, view->RequestId);

        const auto applied = registry.ApplySnapshot(2, RequestIds[0], Snapshot(WireIds[0], 5, 20));
        VERIFY_IS_TRUE(applied.Status == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(applied.Phase == RuntimeClientSubscriptionPhase::Live);
        view = registry.Find(1);
        VERIFY_ARE_EQUAL(std::string{ WireIds[0] }, view->WireSubscriptionId);
        VERIFY_ARE_EQUAL(uint64_t{ 5 }, view->Generation);
        VERIFY_ARE_EQUAL(uint64_t{ 20 }, view->Cursor);
        VERIFY_IS_TRUE(registry.ApplySnapshot(2, RequestIds[0], Snapshot(WireIds[0])).Status ==
                       RuntimeClientSubscriptionStatus::Stale);
    }

    void RuntimeClientSubscriptionTests::ObservesKnownDuplicateAndUnknownNotifications()
    {
        RuntimeClientSubscriptionRegistry registry{ { 1 } };
        MakeLive(registry, 1, 1, 3, RequestIds[0], WireIds[0], 2, 10);

        const auto applied = registry.Observe(3, Notification(WireIds[0], 2, 11));
        VERIFY_IS_TRUE(applied.Disposition == NotificationDisposition::Apply);
        VERIFY_IS_TRUE(applied.Deliver);
        const auto duplicate = registry.Observe(3, Notification(WireIds[0], 2, 11));
        VERIFY_IS_TRUE(duplicate.Disposition == NotificationDisposition::IgnoreDuplicate);

        auto unknownValue = Notification(WireIds[0], 2, 12);
        unknownValue.UnknownKind = "future_optional_event";
        const auto unknown = registry.Observe(3, unknownValue);
        VERIFY_IS_TRUE(unknown.Disposition == NotificationDisposition::IgnoreUnknown);
        VERIFY_IS_FALSE(unknown.Deliver);
        VERIFY_ARE_EQUAL(uint64_t{ 12 }, registry.Find(1)->Cursor);
    }

    void RuntimeClientSubscriptionTests::GapGenerationDriftAndClosedRequireResnapshot()
    {
        RuntimeClientSubscriptionRegistry registry{ { 3 } };
        MakeLive(registry, 1, 1, 4, RequestIds[0], WireIds[0]);
        MakeLive(registry, 2, 1, 4, RequestIds[1], WireIds[1]);
        MakeLive(registry, 3, 1, 4, RequestIds[2], WireIds[2]);

        const auto gap = registry.Observe(4, Notification(WireIds[0], 1, 12));
        VERIFY_IS_TRUE(gap.Disposition == NotificationDisposition::Resnapshot);
        VERIFY_IS_TRUE(registry.Find(1)->Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot);
        const auto drift = registry.Observe(4, Notification(WireIds[1], 2, 11));
        VERIFY_IS_TRUE(drift.Disposition == NotificationDisposition::Resnapshot);
        VERIFY_IS_TRUE(registry.Find(2)->Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot);
        const auto closed = registry.Observe(
            4,
            Notification(WireIds[2], 1, 11, ControlNotificationKind::SubscriptionClosed));
        VERIFY_IS_TRUE(closed.Disposition == NotificationDisposition::Apply);
        VERIFY_IS_TRUE(closed.Deliver);
        VERIFY_IS_TRUE(registry.Find(3)->Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot);
    }

    void RuntimeClientSubscriptionTests::RemovesLiveSubscriptionThroughBoundUnsubscribe()
    {
        RuntimeClientSubscriptionRegistry registry{ { 2 } };
        VERIFY_IS_TRUE(registry.Add(1, 1) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginRemove(1, 1).Status == RuntimeClientSubscriptionStatus::Finalized);

        MakeLive(registry, 2, 3, 6, RequestIds[0], WireIds[0], 4, 9);
        const auto removal = registry.BeginRemove(2, 3);
        VERIFY_IS_TRUE(removal.Action == RuntimeClientSubscriptionRemovalAction::SendUnsubscribe);
        VERIFY_ARE_EQUAL(std::string{ WireIds[0] }, removal.Request->SubscriptionId);
        VERIFY_ARE_EQUAL(uint64_t{ 4 }, removal.Request->Generation);
        VERIFY_IS_TRUE(registry.BeginUnsubscribe(2, 4, 6, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.BeginUnsubscribe(2, 3, 6, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginRemove(2, 3).Action ==
                       RuntimeClientSubscriptionRemovalAction::UnsubscribeInFlight);
        VERIFY_IS_TRUE(registry.CompleteUnsubscribe(7, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.CompleteUnsubscribe(6, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Finalized);
        VERIFY_IS_FALSE(registry.Find(2).has_value());
        VERIFY_IS_TRUE(registry.CompleteUnsubscribe(6, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Stale);

        MakeLive(registry, 1, 4, 6, RequestIds[2], WireIds[1], 5, 10);
        const auto integrationRemoval = registry.BeginRemoveForIntegration(1, 4);
        VERIFY_IS_TRUE(integrationRemoval.Status == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(integrationRemoval.Action ==
                       RuntimeClientSubscriptionRemovalAction::SendUnsubscribe);
        VERIFY_IS_FALSE(integrationRemoval.Request.has_value());
        VERIFY_IS_TRUE(registry.Find(1)->Phase == RuntimeClientSubscriptionPhase::Removing);
        VERIFY_IS_TRUE(registry.BeginUnsubscribe(1, 4, 6, RequestIds[3]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.CompleteUnsubscribe(6, RequestIds[3]) ==
                       RuntimeClientSubscriptionStatus::Finalized);
    }

    void RuntimeClientSubscriptionTests::RemoveDuringSnapshotWaitsThenCleansWireSubscription()
    {
        RuntimeClientSubscriptionRegistry registry{ { 1 } };
        VERIFY_IS_TRUE(registry.Add(1, 2) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 2, 8, RequestIds[0]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginRemove(1, 2).Action ==
                       RuntimeClientSubscriptionRemovalAction::AwaitSnapshot);
        VERIFY_IS_TRUE(registry.BeginRemove(1, 2).Action ==
                       RuntimeClientSubscriptionRemovalAction::AwaitSnapshot);
        const auto applied = registry.ApplySnapshot(8, RequestIds[0], Snapshot(WireIds[0], 3, 5));
        VERIFY_IS_TRUE(applied.NeedsUnsubscribe);
        VERIFY_IS_TRUE(applied.Phase == RuntimeClientSubscriptionPhase::Removing);
        VERIFY_IS_TRUE(registry.BeginRemove(1, 2).Action ==
                       RuntimeClientSubscriptionRemovalAction::SendUnsubscribe);
        VERIFY_IS_TRUE(registry.BeginUnsubscribe(1, 2, 8, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        const auto closed = registry.Observe(
            8,
            Notification(WireIds[0], 3, 6, ControlNotificationKind::SubscriptionClosed));
        VERIFY_IS_TRUE(closed.Finalized);
        VERIFY_IS_FALSE(closed.Deliver);
        VERIFY_IS_FALSE(registry.Find(1).has_value());
        VERIFY_IS_TRUE(registry.CompleteUnsubscribe(8, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Stale);
    }

    void RuntimeClientSubscriptionTests::RemovedLogicalIdRejectsOldSnapshotAfterReuse()
    {
        RuntimeClientSubscriptionRegistry registry{ { 1 } };
        VERIFY_IS_TRUE(registry.Add(1, 1) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 1, 8, RequestIds[0]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginRemove(1, 1).Action ==
                       RuntimeClientSubscriptionRemovalAction::AwaitSnapshot);
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.RetireEpoch(8).Finalized);
        VERIFY_IS_TRUE(registry.Add(1, 2) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 2, 9, RequestIds[0]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginRemove(1, 1).Status == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 1, 9, RequestIds[2]) ==
                       RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.FailRequest(8, RequestIds[0]) == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.ForceNeedsSnapshot(1, 1, 8) == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.ApplySnapshot(8, RequestIds[0], Snapshot(WireIds[0])).Status ==
                       RuntimeClientSubscriptionStatus::Stale);
        const auto view = registry.Find(1);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, view->Revision);
        VERIFY_ARE_EQUAL(uint64_t{ 9 }, view->Epoch);
        VERIFY_ARE_EQUAL(std::string{ RequestIds[0] }, view->RequestId);
        VERIFY_IS_TRUE(view->Phase == RuntimeClientSubscriptionPhase::SnapshotInFlight);
        const auto rebound = registry.FindRequestBinding(9, RequestIds[0]);
        VERIFY_IS_TRUE(rebound.has_value());
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, rebound->Revision);
        VERIFY_IS_TRUE(registry.ApplySnapshot(9, RequestIds[0], Snapshot(WireIds[1])).Status ==
                       RuntimeClientSubscriptionStatus::Succeeded);
    }

    void RuntimeClientSubscriptionTests::RetireEpochResnapshotsDesiredAndFinalizesRemoving()
    {
        RuntimeClientSubscriptionRegistry registry{ { 5 } };
        VERIFY_IS_TRUE(registry.Add(1, 1) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 1, 9, RequestIds[0]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        MakeLive(registry, 2, 1, 9, RequestIds[1], WireIds[1]);
        MakeLive(registry, 3, 1, 9, RequestIds[2], WireIds[2]);
        VERIFY_IS_TRUE(registry.BeginRemove(3, 1).Action ==
                       RuntimeClientSubscriptionRemovalAction::SendUnsubscribe);
        MakeLive(registry, 4, 1, 9, RequestIds[3], WireIds[3]);
        VERIFY_IS_TRUE(registry.BeginRemove(4, 1).Action ==
                       RuntimeClientSubscriptionRemovalAction::SendUnsubscribe);
        VERIFY_IS_TRUE(registry.BeginUnsubscribe(4, 1, 9, RequestIds[4]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        MakeLive(registry, 5, 1, 10, RequestIds[5], WireIds[4]);

        const auto retired = registry.RetireEpoch(9);
        VERIFY_IS_TRUE(retired.Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 2 }, retired.NeedsSnapshot);
        VERIFY_ARE_EQUAL(size_t{ 2 }, retired.Finalized);
        VERIFY_IS_TRUE(registry.Find(1)->Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot);
        VERIFY_IS_TRUE(registry.Find(2)->Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot);
        VERIFY_IS_FALSE(registry.Find(3).has_value());
        VERIFY_IS_FALSE(registry.Find(4).has_value());
        VERIFY_IS_TRUE(registry.Find(5)->Phase == RuntimeClientSubscriptionPhase::Live);
        VERIFY_IS_TRUE(registry.ApplySnapshot(9, RequestIds[0], Snapshot(WireIds[0])).Status ==
                       RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.CompleteUnsubscribe(9, RequestIds[4]) ==
                       RuntimeClientSubscriptionStatus::Stale);
    }

    void RuntimeClientSubscriptionTests::RejectsWireConflictDuplicateRequestAndStaleRevision()
    {
        RuntimeClientSubscriptionRegistry registry{ { 3 } };
        VERIFY_IS_TRUE(registry.Add(1, 2) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.Add(2, 3) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.Add(3, 4) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 2, 11, RequestIds[0]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(2, 3, 11, RequestIds[0]) ==
                       RuntimeClientSubscriptionStatus::DuplicateRequestId);
        VERIFY_IS_TRUE(registry.BeginSnapshot(2, 2, 11, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.BeginSnapshot(2, 3, 11, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);

        auto invalid = Snapshot(WireIds[1], 0, 0);
        VERIFY_IS_TRUE(registry.ApplySnapshot(11, RequestIds[1], invalid).Status ==
                       RuntimeClientSubscriptionStatus::Invalid);
        VERIFY_IS_TRUE(registry.ApplySnapshot(11, RequestIds[0], Snapshot(WireIds[0])).Status ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.ApplySnapshot(11, RequestIds[1], Snapshot(WireIds[0])).Status ==
                       RuntimeClientSubscriptionStatus::WireConflict);
        VERIFY_IS_TRUE(registry.Find(2)->Phase == RuntimeClientSubscriptionPhase::SnapshotInFlight);
    }

    void RuntimeClientSubscriptionTests::StaleEpochAndUnknownWireDoNotMutateCursor()
    {
        RuntimeClientSubscriptionRegistry registry{ { 1 } };
        MakeLive(registry, 1, 1, 12, RequestIds[0], WireIds[0], 1, 7);
        const auto stale = registry.Observe(13, Notification(WireIds[0], 1, 8));
        VERIFY_IS_TRUE(stale.Status == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, registry.Find(1)->Cursor);
        const auto unknown = registry.Observe(12, Notification(WireIds[1], 1, 8));
        VERIFY_IS_TRUE(unknown.Status == RuntimeClientSubscriptionStatus::NotFound);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, registry.Find(1)->Cursor);
    }

    void RuntimeClientSubscriptionTests::FindsSnapshotAndUnsubscribeRequestBindings()
    {
        RuntimeClientSubscriptionRegistry registry{ { 1 } };
        VERIFY_IS_TRUE(registry.Add(1, 2) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 2, 13, RequestIds[0]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_FALSE(registry.FindRequestBinding(12, RequestIds[0]).has_value());
        VERIFY_IS_FALSE(registry.FindRequestBinding(13, RequestIds[1]).has_value());
        const auto snapshot = registry.FindRequestBinding(13, RequestIds[0]);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, snapshot->LogicalId);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, snapshot->Revision);
        VERIFY_IS_TRUE(snapshot->Kind == RuntimeClientSubscriptionRequestKind::Snapshot);

        VERIFY_IS_TRUE(registry.ApplySnapshot(13, RequestIds[0], Snapshot(WireIds[0])).Status ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginRemove(1, 2).Action ==
                       RuntimeClientSubscriptionRemovalAction::SendUnsubscribe);
        VERIFY_IS_TRUE(registry.BeginUnsubscribe(1, 2, 13, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        const auto unsubscribe = registry.FindRequestBinding(13, RequestIds[1]);
        VERIFY_IS_TRUE(unsubscribe.has_value());
        VERIFY_IS_TRUE(unsubscribe->Kind == RuntimeClientSubscriptionRequestKind::Unsubscribe);
        VERIFY_ARE_EQUAL(std::string{ RequestIds[1] }, registry.Find(1)->RequestId);
    }

    void RuntimeClientSubscriptionTests::SnapshotRequestFailureRetriesOrFinalizesRemoval()
    {
        RuntimeClientSubscriptionRegistry registry{ { 2 } };
        VERIFY_IS_TRUE(registry.Add(1, 3) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(1, 3, 14, RequestIds[0]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.FailRequest(15, RequestIds[0]) == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.FindRequestBinding(14, RequestIds[0]).has_value());
        VERIFY_IS_TRUE(registry.FailRequest(14, RequestIds[0]) == RuntimeClientSubscriptionStatus::Succeeded);
        auto view = registry.Find(1);
        VERIFY_IS_TRUE(view->Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot);
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, view->Epoch);
        VERIFY_IS_TRUE(view->RequestId.empty());
        VERIFY_IS_TRUE(registry.FailRequest(14, RequestIds[0]) == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.NextNeedingSnapshot()->LogicalId == 1);

        VERIFY_IS_TRUE(registry.Add(2, 4) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(2, 4, 14, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginRemove(2, 4).Action ==
                       RuntimeClientSubscriptionRemovalAction::AwaitSnapshot);
        VERIFY_IS_TRUE(registry.FailRequest(14, RequestIds[1]) == RuntimeClientSubscriptionStatus::Finalized);
        VERIFY_IS_FALSE(registry.Find(2).has_value());
        VERIFY_IS_TRUE(registry.FailRequest(14, RequestIds[1]) == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.ApplySnapshot(14, RequestIds[1], Snapshot(WireIds[1])).Status ==
                       RuntimeClientSubscriptionStatus::Stale);
    }

    void RuntimeClientSubscriptionTests::UnsubscribeRequestFailurePreservesWireForRetry()
    {
        RuntimeClientSubscriptionRegistry registry{ { 1 } };
        MakeLive(registry, 1, 5, 16, RequestIds[0], WireIds[0], 3, 7);
        VERIFY_IS_TRUE(registry.BeginRemove(1, 5).Action ==
                       RuntimeClientSubscriptionRemovalAction::SendUnsubscribe);
        VERIFY_IS_TRUE(registry.BeginUnsubscribe(1, 5, 16, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.FailRequest(17, RequestIds[1]) == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.FindRequestBinding(16, RequestIds[1]).has_value());
        VERIFY_IS_TRUE(registry.FailRequest(16, RequestIds[1]) == RuntimeClientSubscriptionStatus::Succeeded);
        auto view = registry.Find(1);
        VERIFY_IS_TRUE(view->Phase == RuntimeClientSubscriptionPhase::Removing);
        VERIFY_ARE_EQUAL(std::string{ WireIds[0] }, view->WireSubscriptionId);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, view->Generation);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, view->Cursor);
        VERIFY_ARE_EQUAL(uint64_t{ 16 }, view->Epoch);
        VERIFY_IS_TRUE(view->RequestId.empty());
        VERIFY_IS_TRUE(registry.FailRequest(16, RequestIds[1]) == RuntimeClientSubscriptionStatus::Stale);

        VERIFY_IS_TRUE(registry.BeginRemove(1, 5).Action ==
                       RuntimeClientSubscriptionRemovalAction::SendUnsubscribe);
        VERIFY_IS_TRUE(registry.BeginUnsubscribe(1, 5, 16, RequestIds[2]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.CompleteUnsubscribe(16, RequestIds[2]) ==
                       RuntimeClientSubscriptionStatus::Finalized);
    }

    void RuntimeClientSubscriptionTests::ForceNeedsSnapshotRejectsStaleAndFinalizesRemoval()
    {
        RuntimeClientSubscriptionRegistry registry{ { 3 } };
        MakeLive(registry, 1, 6, 18, RequestIds[0], WireIds[0]);
        VERIFY_IS_TRUE(registry.ForceNeedsSnapshot(1, 5, 18) == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.ForceNeedsSnapshot(1, 6, 17) == RuntimeClientSubscriptionStatus::Stale);
        const auto unchanged = registry.Find(1);
        VERIFY_IS_TRUE(unchanged->Phase == RuntimeClientSubscriptionPhase::Live);
        VERIFY_ARE_EQUAL(uint64_t{ 18 }, unchanged->Epoch);
        VERIFY_ARE_EQUAL(std::string{ WireIds[0] }, unchanged->WireSubscriptionId);
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, unchanged->Generation);
        VERIFY_ARE_EQUAL(uint64_t{ 10 }, unchanged->Cursor);
        VERIFY_IS_TRUE(registry.ForceNeedsSnapshot(1, 6, 18) == RuntimeClientSubscriptionStatus::Succeeded);
        const auto live = registry.Find(1);
        VERIFY_IS_TRUE(live->Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot);
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, live->Epoch);
        VERIFY_IS_TRUE(live->WireSubscriptionId.empty());
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, live->Generation);
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, live->Cursor);
        VERIFY_IS_TRUE(registry.Observe(18, Notification(WireIds[0], 1, 11)).Status ==
                       RuntimeClientSubscriptionStatus::NotFound);
        VERIFY_IS_TRUE(registry.NextNeedingSnapshot()->LogicalId == 1);
        VERIFY_IS_TRUE(registry.ForceNeedsSnapshot(1, 6, 18) == RuntimeClientSubscriptionStatus::Stale);
        VERIFY_IS_TRUE(registry.Find(1)->Phase == RuntimeClientSubscriptionPhase::NeedsSnapshot);

        VERIFY_IS_TRUE(registry.Add(2, 7) == RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.BeginSnapshot(2, 7, 18, RequestIds[1]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.ForceNeedsSnapshot(2, 7, 18) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_FALSE(registry.FindRequestBinding(18, RequestIds[1]).has_value());
        VERIFY_IS_TRUE(registry.ApplySnapshot(18, RequestIds[1], Snapshot(WireIds[1])).Status ==
                       RuntimeClientSubscriptionStatus::Stale);

        MakeLive(registry, 3, 8, 18, RequestIds[2], WireIds[2]);
        VERIFY_IS_TRUE(registry.BeginRemove(3, 8).Action ==
                       RuntimeClientSubscriptionRemovalAction::SendUnsubscribe);
        VERIFY_IS_TRUE(registry.BeginUnsubscribe(3, 8, 18, RequestIds[3]) ==
                       RuntimeClientSubscriptionStatus::Succeeded);
        VERIFY_IS_TRUE(registry.ForceNeedsSnapshot(3, 8, 18) ==
                       RuntimeClientSubscriptionStatus::Finalized);
        VERIFY_IS_FALSE(registry.Find(3).has_value());
        VERIFY_IS_TRUE(registry.CompleteUnsubscribe(18, RequestIds[3]) ==
                       RuntimeClientSubscriptionStatus::Stale);
    }
}
