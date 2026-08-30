// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeTerminalInputLease.h"

#include <limits>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr uint64_t Generation{ 7 };
        constexpr std::string_view ClientA{
            "97000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view ClientB{
            "97000000-0000-4000-8000-000000000002"
        };

        RuntimeTerminalInputLease _lease(
            const uint32_t duration = 100,
            const RuntimeTerminalLeaseRevision initialRevision = 0)
        {
            return RuntimeTerminalInputLease{
                {
                    Generation,
                    duration,
                    initialRevision,
                }
            };
        }
    }

    class RuntimeTerminalInputLeaseTests
    {
        TEST_CLASS(RuntimeTerminalInputLeaseTests);

        TEST_METHOD(RejectsInvalidConfigurationClientAndGeneration);
        TEST_METHOD(AcquireRenewAndBusyPreserveOneController);
        TEST_METHOD(AuthorizationRequiresOwnerGenerationAndLeaseRevision);
        TEST_METHOD(ExactExpiryAndReacquirePreventAbaInput);
        TEST_METHOD(ReleaseAndDisconnectAreOwnerScopedAndIdempotent);
        TEST_METHOD(ClockRegressionSaturationAndRevisionExhaustionFailClosed);
    };

    void RuntimeTerminalInputLeaseTests::RejectsInvalidConfigurationClientAndGeneration()
    {
        const std::array configurations{
            RuntimeTerminalInputLeaseConfiguration{ 0, 100, 0 },
            RuntimeTerminalInputLeaseConfiguration{ Generation, 0, 0 },
            RuntimeTerminalInputLeaseConfiguration{
                Generation,
                MaximumRuntimeTerminalLeaseDurationMilliseconds + 1,
                0,
            },
        };
        for (const auto& configuration : configurations)
        {
            RuntimeTerminalInputLease lease{ configuration };
            VERIFY_IS_FALSE(lease.Valid());
            VERIFY_IS_TRUE(
                lease.Acquire(ClientA, Generation, 1).Status ==
                RuntimeTerminalInputLeaseStatus::InvalidConfiguration);
        }

        auto lease = _lease();
        VERIFY_IS_TRUE(
            lease.Acquire("not-a-client", Generation, 1).Status ==
            RuntimeTerminalInputLeaseStatus::InvalidClient);
        VERIFY_IS_TRUE(
            lease.Acquire(ClientA, Generation + 1, 1).Status ==
            RuntimeTerminalInputLeaseStatus::StaleGeneration);
        VERIFY_IS_FALSE(lease.Current().has_value());
        VERIFY_ARE_EQUAL(RuntimeTerminalLeaseRevision{ 0 }, lease.LastIssuedRevision());
    }

    void RuntimeTerminalInputLeaseTests::AcquireRenewAndBusyPreserveOneController()
    {
        auto lease = _lease();
        const auto acquired = lease.Acquire(ClientA, Generation, 10);
        VERIFY_IS_TRUE(acquired.Status == RuntimeTerminalInputLeaseStatus::Acquired);
        VERIFY_IS_TRUE(acquired.Lease && acquired.Lease->Owned());
        if (!acquired.Lease)
        {
            return;
        }
        VERIFY_ARE_EQUAL(std::string{ ClientA }, acquired.Lease->OwnerClientInstanceId);
        VERIFY_ARE_EQUAL(RuntimeTerminalLeaseRevision{ 1 }, acquired.Lease->LeaseRevision);
        VERIFY_ARE_EQUAL(RuntimeTerminalLeaseMilliseconds{ 110 }, acquired.Lease->ExpiresAt);

        const auto busy = lease.Acquire(ClientB, Generation, 11);
        VERIFY_IS_TRUE(busy.Status == RuntimeTerminalInputLeaseStatus::Busy);
        VERIFY_IS_TRUE(busy.Lease.has_value());
        VERIFY_ARE_EQUAL(std::string{ ClientA }, busy.Lease->OwnerClientInstanceId);

        const auto renewed = lease.Acquire(ClientA, Generation, 20);
        VERIFY_IS_TRUE(renewed.Status == RuntimeTerminalInputLeaseStatus::Renewed);
        VERIFY_ARE_EQUAL(RuntimeTerminalLeaseRevision{ 1 }, renewed.Lease->LeaseRevision);
        VERIFY_ARE_EQUAL(RuntimeTerminalLeaseMilliseconds{ 120 }, renewed.Lease->ExpiresAt);
        VERIFY_ARE_EQUAL(RuntimeTerminalLeaseRevision{ 1 }, lease.LastIssuedRevision());
    }

    void RuntimeTerminalInputLeaseTests::AuthorizationRequiresOwnerGenerationAndLeaseRevision()
    {
        auto lease = _lease();
        const auto acquired = lease.Acquire(ClientA, Generation, 10);
        VERIFY_IS_TRUE(acquired.Lease.has_value());
        if (!acquired.Lease)
        {
            return;
        }
        const auto revision = acquired.Lease->LeaseRevision;

        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientA, Generation, revision, 11).Status ==
            RuntimeTerminalInputLeaseStatus::Authorized);
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientB, Generation, revision, 12).Status ==
            RuntimeTerminalInputLeaseStatus::NotOwner);
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientA, Generation + 1, revision, 13).Status ==
            RuntimeTerminalInputLeaseStatus::StaleGeneration);
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientA, Generation, revision + 1, 14).Status ==
            RuntimeTerminalInputLeaseStatus::StaleLease);
        VERIFY_IS_TRUE(lease.Current().has_value());
    }

    void RuntimeTerminalInputLeaseTests::ExactExpiryAndReacquirePreventAbaInput()
    {
        auto lease = _lease();
        const auto first = lease.Acquire(ClientA, Generation, 10);
        VERIFY_IS_TRUE(first.Lease.has_value());
        if (!first.Lease)
        {
            return;
        }
        const auto firstRevision = first.Lease->LeaseRevision;
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientA, Generation, firstRevision, 109).Status ==
            RuntimeTerminalInputLeaseStatus::Authorized);
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientA, Generation, firstRevision, 110).Status ==
            RuntimeTerminalInputLeaseStatus::Expired);
        VERIFY_IS_FALSE(lease.Current().has_value());

        const auto second = lease.Acquire(ClientB, Generation, 110);
        VERIFY_IS_TRUE(second.Lease.has_value());
        if (!second.Lease)
        {
            return;
        }
        VERIFY_ARE_EQUAL(RuntimeTerminalLeaseRevision{ 2 }, second.Lease->LeaseRevision);
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientA, Generation, firstRevision, 111).Status ==
            RuntimeTerminalInputLeaseStatus::NotOwner);
        VERIFY_IS_TRUE(
            lease.Release(
                ClientB,
                Generation,
                second.Lease->LeaseRevision,
                112).Status == RuntimeTerminalInputLeaseStatus::Released);

        const auto third = lease.Acquire(ClientA, Generation, 113);
        VERIFY_IS_TRUE(third.Lease.has_value());
        if (!third.Lease)
        {
            return;
        }
        VERIFY_ARE_EQUAL(RuntimeTerminalLeaseRevision{ 3 }, third.Lease->LeaseRevision);
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientA, Generation, firstRevision, 114).Status ==
            RuntimeTerminalInputLeaseStatus::StaleLease);
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(
                ClientA,
                Generation,
                third.Lease->LeaseRevision,
                115).Status == RuntimeTerminalInputLeaseStatus::Authorized);
    }

    void RuntimeTerminalInputLeaseTests::ReleaseAndDisconnectAreOwnerScopedAndIdempotent()
    {
        auto lease = _lease();
        const auto acquired = lease.Acquire(ClientA, Generation, 10);
        VERIFY_IS_TRUE(acquired.Lease.has_value());
        if (!acquired.Lease)
        {
            return;
        }
        const auto revision = acquired.Lease->LeaseRevision;
        VERIFY_IS_TRUE(
            lease.Release(ClientB, Generation, revision, 11).Status ==
            RuntimeTerminalInputLeaseStatus::NotOwner);
        VERIFY_IS_TRUE(
            lease.Release(ClientA, Generation, revision + 1, 12).Status ==
            RuntimeTerminalInputLeaseStatus::StaleLease);
        VERIFY_IS_TRUE(
            lease.Disconnect(ClientB, 13).Status ==
            RuntimeTerminalInputLeaseStatus::NotOwner);
        VERIFY_IS_TRUE(
            lease.Release(ClientA, Generation, revision, 14).Status ==
            RuntimeTerminalInputLeaseStatus::Released);
        VERIFY_IS_TRUE(
            lease.Release(ClientA, Generation, revision, 15).Status ==
            RuntimeTerminalInputLeaseStatus::AlreadyReleased);

        const auto reacquired = lease.Acquire(ClientA, Generation, 16);
        VERIFY_IS_TRUE(reacquired.Status == RuntimeTerminalInputLeaseStatus::Acquired);
        VERIFY_IS_TRUE(
            lease.Disconnect(ClientA, 17).Status ==
            RuntimeTerminalInputLeaseStatus::Released);
        VERIFY_IS_FALSE(lease.Current().has_value());
    }

    void RuntimeTerminalInputLeaseTests::ClockRegressionSaturationAndRevisionExhaustionFailClosed()
    {
        auto lease = _lease();
        const auto acquired = lease.Acquire(ClientA, Generation, 100);
        VERIFY_IS_TRUE(acquired.Lease.has_value());
        if (!acquired.Lease)
        {
            return;
        }
        const auto revision = acquired.Lease->LeaseRevision;
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientA, Generation, revision, 99).Status ==
            RuntimeTerminalInputLeaseStatus::ClockRegression);
        VERIFY_IS_TRUE(
            lease.AuthorizeInput(ClientA, Generation, revision, 100).Status ==
            RuntimeTerminalInputLeaseStatus::Authorized);

        const auto maximum =
            (std::numeric_limits<RuntimeTerminalLeaseMilliseconds>::max)();
        auto saturated = _lease(10);
        const auto nearMaximum = saturated.Acquire(
            ClientA,
            Generation,
            maximum - 5);
        VERIFY_IS_TRUE(nearMaximum.Status == RuntimeTerminalInputLeaseStatus::Acquired);
        VERIFY_IS_TRUE(nearMaximum.Lease.has_value());
        if (!nearMaximum.Lease)
        {
            return;
        }
        VERIFY_ARE_EQUAL(maximum, nearMaximum.Lease->ExpiresAt);
        VERIFY_IS_TRUE(
            saturated.AuthorizeInput(
                ClientA,
                Generation,
                nearMaximum.Lease->LeaseRevision,
                maximum).Status == RuntimeTerminalInputLeaseStatus::Expired);

        auto exhaustedClock = _lease();
        VERIFY_IS_TRUE(
            exhaustedClock.Acquire(ClientA, Generation, maximum).Status ==
            RuntimeTerminalInputLeaseStatus::ClockExhausted);
        auto exhaustedRevision = _lease(
            100,
            (std::numeric_limits<RuntimeTerminalLeaseRevision>::max)());
        VERIFY_IS_TRUE(
            exhaustedRevision.Acquire(ClientA, Generation, 1).Status ==
            RuntimeTerminalInputLeaseStatus::RevisionExhausted);
    }
}
