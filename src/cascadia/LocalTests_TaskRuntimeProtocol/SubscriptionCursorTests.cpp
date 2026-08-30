// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "SubscriptionCursor.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        constexpr std::string_view SubscriptionId{ "40000000-0000-4000-8000-000000000004" };

        ControlNotification _notification(
            const uint64_t generation,
            const uint64_t sequence,
            const ControlNotificationKind kind = ControlNotificationKind::ProjectionChanged)
        {
            ControlNotification notification;
            notification.SubscriptionId = SubscriptionId;
            notification.Generation = generation;
            notification.Sequence = sequence;
            notification.Kind = kind;
            return notification;
        }
    }

    class SubscriptionCursorTests
    {
        TEST_CLASS(SubscriptionCursorTests);

        TEST_METHOD(SnapshotAndStrictlyNewNotificationsAdvanceCursor);
        TEST_METHOD(DuplicatesAtOrBelowCursorAreIgnored);
        TEST_METHOD(GapSubscriptionOrGenerationMismatchRequiresResnapshot);
        TEST_METHOD(SubscriptionClosedAppliesThenRequiresResnapshot);
        TEST_METHOD(InvalidSnapshotAndNotificationDoNotBecomeSynchronized);
        TEST_METHOD(UnknownOptionalNotificationAdvancesCursorWithoutApplyingPayload);
    };

    void SubscriptionCursorTests::SnapshotAndStrictlyNewNotificationsAdvanceCursor()
    {
        SubscriptionCursor cursor;
        VERIFY_IS_TRUE(cursor.ApplySnapshot(std::string{ SubscriptionId }, 2, 10));
        VERIFY_IS_TRUE(cursor.State() == SubscriptionCursorState::Synchronized);
        VERIFY_ARE_EQUAL(uint64_t{ 10 }, cursor.Cursor());
        VERIFY_IS_TRUE(cursor.Observe(_notification(2, 11)) == NotificationDisposition::Apply);
        VERIFY_ARE_EQUAL(uint64_t{ 11 }, cursor.Cursor());
        VERIFY_IS_TRUE(cursor.Observe(_notification(2, 12)) == NotificationDisposition::Apply);
        VERIFY_ARE_EQUAL(uint64_t{ 12 }, cursor.Cursor());
    }

    void SubscriptionCursorTests::DuplicatesAtOrBelowCursorAreIgnored()
    {
        SubscriptionCursor cursor;
        VERIFY_IS_TRUE(cursor.ApplySnapshot(std::string{ SubscriptionId }, 1, 8));
        VERIFY_IS_TRUE(cursor.Observe(_notification(1, 8)) == NotificationDisposition::IgnoreDuplicate);
        VERIFY_IS_TRUE(cursor.Observe(_notification(1, 7)) == NotificationDisposition::IgnoreDuplicate);
        VERIFY_ARE_EQUAL(uint64_t{ 8 }, cursor.Cursor());
        VERIFY_IS_TRUE(cursor.State() == SubscriptionCursorState::Synchronized);
    }

    void SubscriptionCursorTests::GapSubscriptionOrGenerationMismatchRequiresResnapshot()
    {
        SubscriptionCursor cursor;
        VERIFY_IS_TRUE(cursor.ApplySnapshot(std::string{ SubscriptionId }, 1, 3));
        VERIFY_IS_TRUE(cursor.Observe(_notification(1, 5)) == NotificationDisposition::Resnapshot);
        VERIFY_IS_TRUE(cursor.State() == SubscriptionCursorState::ResnapshotRequired);

        cursor.Reset();
        VERIFY_IS_TRUE(cursor.ApplySnapshot(std::string{ SubscriptionId }, 1, 3));
        VERIFY_IS_TRUE(cursor.Observe(_notification(2, 4)) == NotificationDisposition::Resnapshot);

        cursor.Reset();
        VERIFY_IS_TRUE(cursor.ApplySnapshot(std::string{ SubscriptionId }, 1, 3));
        auto other = _notification(1, 4);
        other.SubscriptionId = "50000000-0000-4000-8000-000000000005";
        VERIFY_IS_TRUE(cursor.Observe(other) == NotificationDisposition::Resnapshot);
    }

    void SubscriptionCursorTests::SubscriptionClosedAppliesThenRequiresResnapshot()
    {
        SubscriptionCursor cursor;
        VERIFY_IS_TRUE(cursor.ApplySnapshot(std::string{ SubscriptionId }, 4, 20));
        const auto disposition = cursor.Observe(
            _notification(4, 21, ControlNotificationKind::SubscriptionClosed));
        VERIFY_IS_TRUE(disposition == NotificationDisposition::Apply);
        VERIFY_ARE_EQUAL(uint64_t{ 21 }, cursor.Cursor());
        VERIFY_IS_TRUE(cursor.State() == SubscriptionCursorState::ResnapshotRequired);
    }

    void SubscriptionCursorTests::InvalidSnapshotAndNotificationDoNotBecomeSynchronized()
    {
        SubscriptionCursor cursor;
        VERIFY_IS_FALSE(cursor.ApplySnapshot("not-a-uuid", 1, 0));
        VERIFY_IS_FALSE(cursor.ApplySnapshot(std::string{ SubscriptionId }, 0, 0));
        auto invalid = _notification(1, 0);
        VERIFY_IS_TRUE(cursor.Observe(invalid) == NotificationDisposition::Invalid);
        VERIFY_IS_TRUE(cursor.State() == SubscriptionCursorState::Empty);

        cursor.Reset();
        VERIFY_IS_TRUE(cursor.SubscriptionId().empty());
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, cursor.Generation());
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, cursor.Cursor());
    }

    void SubscriptionCursorTests::UnknownOptionalNotificationAdvancesCursorWithoutApplyingPayload()
    {
        SubscriptionCursor cursor;
        VERIFY_IS_TRUE(cursor.ApplySnapshot(std::string{ SubscriptionId }, 3, 5));
        auto unknown = _notification(3, 6);
        unknown.UnknownKind = "future_optional_event";
        VERIFY_IS_TRUE(cursor.Observe(unknown) == NotificationDisposition::IgnoreUnknown);
        VERIFY_ARE_EQUAL(uint64_t{ 6 }, cursor.Cursor());
        VERIFY_IS_TRUE(cursor.State() == SubscriptionCursorState::Synchronized);
        VERIFY_IS_TRUE(cursor.Observe(_notification(3, 7)) == NotificationDisposition::Apply);
    }
}
