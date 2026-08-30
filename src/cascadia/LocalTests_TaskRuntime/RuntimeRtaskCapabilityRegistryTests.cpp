// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRuntimeRtaskCapabilityTokenSource.h"
#include "RuntimeRtaskCapabilityRegistry.h"

#include <limits>
#include <set>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr std::string_view TaskA{
            "ab000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view TaskB{
            "ab000000-0000-4000-8000-000000000002"
        };
        constexpr std::string_view RunA{
            "aa000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view RunB{
            "aa000000-0000-4000-8000-000000000002"
        };
        constexpr std::string_view RunC{
            "aa000000-0000-4000-8000-000000000003"
        };

        std::string _token(const char value)
        {
            return std::string(RtaskCapabilityBytes, value);
        }

        RtaskMethodSet _methods(
            const RtaskMethod first,
            const std::optional<RtaskMethod> second = std::nullopt)
        {
            return RtaskMethodBit(first) |
                   (second ? RtaskMethodBit(*second) : RtaskMethodSet{});
        }

        std::shared_ptr<FakeRuntimeRtaskCapabilityTokenSource> _source(
            std::initializer_list<std::string> tokens)
        {
            auto source = std::make_shared<FakeRuntimeRtaskCapabilityTokenSource>();
            for (auto token : tokens)
            {
                source->Tokens.emplace_back(std::move(token));
            }
            return source;
        }

        RuntimeRtaskCapabilityRegistryConfiguration _configuration(
            const size_t maximumCapabilities = 4,
            const uint32_t maximumLifetime = 1000)
        {
            return {
                maximumCapabilities,
                maximumLifetime,
            };
        }
    }

    class RuntimeRtaskCapabilityRegistryTests
    {
        TEST_CLASS(RuntimeRtaskCapabilityRegistryTests);

        TEST_METHOD(RejectsInvalidConfigurationScopeAndLifetime);
        TEST_METHOD(AuthorizationBindsTokenRunAndMethod);
        TEST_METHOD(IssueRenewReplayAndExpiryAreIdempotent);
        TEST_METHOD(InvalidationCapacityAndExpiredPurgeAreRunScoped);
        TEST_METHOD(ClockTokenSourceAndCollisionFailuresAreBounded);
        TEST_METHOD(SystemTokenSourceProducesUniqueValidSecrets);
    };

    void RuntimeRtaskCapabilityRegistryTests::RejectsInvalidConfigurationScopeAndLifetime()
    {
        const auto validSource = _source({ _token('a') });
        const std::array configurations{
            RuntimeRtaskCapabilityRegistryConfiguration{ 0, 100 },
            RuntimeRtaskCapabilityRegistryConfiguration{
                MaximumRuntimeRtaskCapabilities + 1,
                100,
            },
            RuntimeRtaskCapabilityRegistryConfiguration{ 1, 0 },
            RuntimeRtaskCapabilityRegistryConfiguration{
                1,
                MaximumRuntimeRtaskCapabilityLifetimeMilliseconds + 1,
            },
        };
        for (const auto& configuration : configurations)
        {
            RuntimeRtaskCapabilityRegistry registry{ configuration, validSource };
            VERIFY_IS_FALSE(registry.Valid());
            VERIFY_IS_TRUE(
                registry.IssueOrRenew(
                    TaskA,
                    RunA,
                    RtaskMethodBit(RtaskMethod::StatusInProgress),
                    10,
                    1).Status == RuntimeRtaskCapabilityStatus::InvalidConfiguration);
        }

        RuntimeRtaskCapabilityRegistry missingSource{ _configuration(), nullptr };
        VERIFY_IS_FALSE(missingSource.Valid());

        auto source = _source({ _token('a') });
        RuntimeRtaskCapabilityRegistry registry{ _configuration(), source };
        VERIFY_IS_TRUE(registry.Valid());
        VERIFY_IS_TRUE(
            registry.IssueOrRenew(
                "not-a-task",
                RunA,
                RtaskMethodBit(RtaskMethod::StatusInProgress),
                100,
                1).Status == RuntimeRtaskCapabilityStatus::InvalidTask);
        VERIFY_IS_TRUE(
            registry.IssueOrRenew(
                TaskA,
                "not-a-run",
                RtaskMethodBit(RtaskMethod::StatusInProgress),
                100,
                1).Status == RuntimeRtaskCapabilityStatus::InvalidRun);
        VERIFY_IS_TRUE(
            registry.IssueOrRenew(TaskA, RunA, 0, 100, 1).Status ==
            RuntimeRtaskCapabilityStatus::InvalidMethodSet);
        VERIFY_IS_TRUE(
            registry.IssueOrRenew(TaskA, RunA, AllRtaskMethods | (1u << 8), 100, 1).Status ==
            RuntimeRtaskCapabilityStatus::InvalidMethodSet);
        VERIFY_IS_TRUE(
            registry.IssueOrRenew(
                TaskA,
                RunA,
                RtaskMethodBit(RtaskMethod::StatusInProgress),
                0,
                1).Status == RuntimeRtaskCapabilityStatus::InvalidLifetime);
        VERIFY_IS_TRUE(
            registry.IssueOrRenew(
                TaskA,
                RunA,
                RtaskMethodBit(RtaskMethod::StatusInProgress),
                1001,
                1).Status == RuntimeRtaskCapabilityStatus::InvalidLifetime);
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.Size());
        VERIFY_ARE_EQUAL(size_t{ 0 }, source->Calls);
    }

    void RuntimeRtaskCapabilityRegistryTests::AuthorizationBindsTokenRunAndMethod()
    {
        auto source = _source({ _token('a') });
        RuntimeRtaskCapabilityRegistry registry{ _configuration(), source };
        const auto methods = _methods(
            RtaskMethod::StatusInProgress,
            RtaskMethod::Result);
        const auto issued = registry.IssueOrRenew(TaskA, RunA, methods, 100, 10);
        VERIFY_IS_TRUE(issued.Status == RuntimeRtaskCapabilityStatus::Issued);
        VERIFY_IS_TRUE(issued.Grant.has_value());
        if (!issued.Grant)
        {
            return;
        }
        const auto capability = issued.Grant->Capability;
        VERIFY_ARE_EQUAL(std::string{ TaskA }, issued.Grant->TaskId);
        VERIFY_IS_TRUE(
            registry.Authorize(
                capability,
                RunA,
                RtaskMethod::StatusInProgress,
                10).Status == RuntimeRtaskCapabilityStatus::Authorized);
        const auto authorized = registry.Authorize(
            capability,
            RunA,
            RtaskMethod::Result,
            11);
        VERIFY_IS_TRUE(authorized.Status == RuntimeRtaskCapabilityStatus::Authorized);
        VERIFY_IS_TRUE(authorized.Binding.has_value());
        if (!authorized.Binding)
        {
            return;
        }
        VERIFY_ARE_EQUAL(std::string{ TaskA }, authorized.Binding->TaskId);
        VERIFY_ARE_EQUAL(std::string{ RunA }, authorized.Binding->RunId);
        VERIFY_IS_TRUE(
            registry.Authorize(
                capability,
                RunB,
                RtaskMethod::Result,
                11).Status == RuntimeRtaskCapabilityStatus::WrongRun);
        VERIFY_IS_TRUE(
            registry.Authorize(
                capability,
                RunA,
                RtaskMethod::StatusInReview,
                11).Status == RuntimeRtaskCapabilityStatus::WrongMethod);
        VERIFY_IS_TRUE(
            registry.Authorize(
                _token('b'),
                RunA,
                RtaskMethod::Result,
                11).Status == RuntimeRtaskCapabilityStatus::Missing);
        VERIFY_IS_TRUE(
            registry.Authorize(
                "invalid-token",
                RunA,
                RtaskMethod::Result,
                11).Status == RuntimeRtaskCapabilityStatus::InvalidToken);
        VERIFY_IS_TRUE(
            registry.Authorize(
                capability,
                RunA,
                RtaskMethod::Count,
                11).Status == RuntimeRtaskCapabilityStatus::InvalidMethod);
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.Size());
    }

    void RuntimeRtaskCapabilityRegistryTests::IssueRenewReplayAndExpiryAreIdempotent()
    {
        auto source = _source({ _token('a'), _token('b') });
        RuntimeRtaskCapabilityRegistry registry{ _configuration(), source };
        const auto methods = _methods(RtaskMethod::StatusBlocked);
        const auto issued = registry.IssueOrRenew(TaskA, RunA, methods, 100, 100);
        VERIFY_IS_TRUE(issued.Grant.has_value());
        if (!issued.Grant)
        {
            return;
        }
        const auto capability = issued.Grant->Capability;
        VERIFY_ARE_EQUAL(RuntimeRtaskMilliseconds{ 200 }, issued.Grant->ExpiresAt);

        const auto replayed = registry.IssueOrRenew(TaskA, RunA, methods, 100, 100);
        VERIFY_IS_TRUE(replayed.Status == RuntimeRtaskCapabilityStatus::Replayed);
        VERIFY_IS_TRUE(replayed.Grant.has_value());
        VERIFY_ARE_EQUAL(capability, replayed.Grant->Capability);
        VERIFY_ARE_EQUAL(RuntimeRtaskMilliseconds{ 200 }, replayed.Grant->ExpiresAt);
        VERIFY_ARE_EQUAL(size_t{ 1 }, source->Calls);

        const auto renewed = registry.IssueOrRenew(TaskA, RunA, methods, 100, 150);
        VERIFY_IS_TRUE(renewed.Status == RuntimeRtaskCapabilityStatus::Renewed);
        VERIFY_ARE_EQUAL(capability, renewed.Grant->Capability);
        VERIFY_ARE_EQUAL(RuntimeRtaskMilliseconds{ 250 }, renewed.Grant->ExpiresAt);
        VERIFY_ARE_EQUAL(size_t{ 1 }, source->Calls);
        VERIFY_IS_TRUE(
            registry.IssueOrRenew(
                TaskA,
                RunA,
                _methods(RtaskMethod::Result),
                100,
                150).Status == RuntimeRtaskCapabilityStatus::ScopeConflict);
        VERIFY_IS_TRUE(
            registry.IssueOrRenew(
                TaskB,
                RunA,
                methods,
                100,
                150).Status == RuntimeRtaskCapabilityStatus::ScopeConflict);
        VERIFY_IS_TRUE(
            registry.Authorize(
                capability,
                RunA,
                RtaskMethod::StatusBlocked,
                249).Status == RuntimeRtaskCapabilityStatus::Authorized);
        VERIFY_IS_TRUE(
            registry.Authorize(
                capability,
                RunA,
                RtaskMethod::StatusBlocked,
                250).Status == RuntimeRtaskCapabilityStatus::Expired);
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.Size());

        const auto replacement = registry.IssueOrRenew(TaskA, RunA, methods, 100, 250);
        VERIFY_IS_TRUE(replacement.Status == RuntimeRtaskCapabilityStatus::Issued);
        VERIFY_IS_TRUE(replacement.Grant.has_value());
        VERIFY_ARE_EQUAL(_token('b'), replacement.Grant->Capability);
        VERIFY_ARE_EQUAL(size_t{ 2 }, source->Calls);
    }

    void RuntimeRtaskCapabilityRegistryTests::InvalidationCapacityAndExpiredPurgeAreRunScoped()
    {
        auto source = _source({ _token('a'), _token('b'), _token('c'), _token('d') });
        RuntimeRtaskCapabilityRegistry registry{ _configuration(2, 100), source };
        const auto methods = _methods(RtaskMethod::StatusInReview);
        const auto first = registry.IssueOrRenew(TaskA, RunA, methods, 50, 0);
        const auto second = registry.IssueOrRenew(TaskA, RunB, methods, 100, 0);
        VERIFY_IS_TRUE(first.Grant.has_value());
        VERIFY_IS_TRUE(second.Grant.has_value());
        VERIFY_IS_TRUE(
            registry.IssueOrRenew(TaskA, RunC, methods, 100, 0).Status ==
            RuntimeRtaskCapabilityStatus::CapacityExceeded);

        const auto invalidated = registry.InvalidateRun(RunA);
        VERIFY_IS_TRUE(invalidated.Status == RuntimeRtaskCapabilityStatus::Invalidated);
        VERIFY_ARE_EQUAL(size_t{ 1 }, invalidated.InvalidatedCapabilities);
        VERIFY_IS_TRUE(
            registry.Authorize(
                first.Grant->Capability,
                RunA,
                RtaskMethod::StatusInReview,
                1).Status == RuntimeRtaskCapabilityStatus::Missing);
        VERIFY_IS_TRUE(
            registry.Authorize(
                second.Grant->Capability,
                RunB,
                RtaskMethod::StatusInReview,
                1).Status == RuntimeRtaskCapabilityStatus::Authorized);

        const auto third = registry.IssueOrRenew(TaskA, RunC, methods, 50, 1);
        VERIFY_IS_TRUE(third.Status == RuntimeRtaskCapabilityStatus::Issued);
        VERIFY_ARE_EQUAL(size_t{ 2 }, registry.Size());
        VERIFY_IS_TRUE(
            registry.InvalidateCapability(third.Grant->Capability).Status ==
            RuntimeRtaskCapabilityStatus::Invalidated);
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.Size());

        const auto purgedReplacement = registry.IssueOrRenew(TaskA, RunA, methods, 50, 100);
        VERIFY_IS_TRUE(
            purgedReplacement.Status == RuntimeRtaskCapabilityStatus::Issued);
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.Size());
        VERIFY_IS_TRUE(
            registry.InvalidateRun(RunB).Status == RuntimeRtaskCapabilityStatus::Missing);
    }

    void RuntimeRtaskCapabilityRegistryTests::ClockTokenSourceAndCollisionFailuresAreBounded()
    {
        auto clockSource = _source({ _token('a') });
        RuntimeRtaskCapabilityRegistry clockRegistry{ _configuration(), clockSource };
        const auto issued = clockRegistry.IssueOrRenew(
            TaskA,
            RunA,
            _methods(RtaskMethod::Result),
            100,
            100);
        VERIFY_IS_TRUE(issued.Grant.has_value());
        VERIFY_IS_TRUE(
            clockRegistry.Authorize(
                issued.Grant->Capability,
                RunA,
                RtaskMethod::Result,
                99).Status == RuntimeRtaskCapabilityStatus::ClockRegression);

        auto exhaustedSource = _source({ _token('a') });
        RuntimeRtaskCapabilityRegistry exhausted{ _configuration(), exhaustedSource };
        VERIFY_IS_TRUE(
            exhausted.IssueOrRenew(
                TaskA,
                RunA,
                _methods(RtaskMethod::Result),
                1,
                (std::numeric_limits<uint64_t>::max)()).Status ==
            RuntimeRtaskCapabilityStatus::ClockExhausted);

        auto collisionSource = std::make_shared<FakeRuntimeRtaskCapabilityTokenSource>();
        for (size_t index = 0; index < 9; ++index)
        {
            collisionSource->Tokens.emplace_back(_token('a'));
        }
        RuntimeRtaskCapabilityRegistry collisions{ _configuration(), collisionSource };
        VERIFY_IS_TRUE(
            collisions.IssueOrRenew(
                TaskA,
                RunA,
                _methods(RtaskMethod::Result),
                100,
                1).Status == RuntimeRtaskCapabilityStatus::Issued);
        VERIFY_IS_TRUE(
            collisions.IssueOrRenew(
                TaskA,
                RunB,
                _methods(RtaskMethod::Result),
                100,
                1).Status == RuntimeRtaskCapabilityStatus::TokenCollision);
        VERIFY_ARE_EQUAL(size_t{ 9 }, collisionSource->Calls);
        VERIFY_ARE_EQUAL(size_t{ 1 }, collisions.Size());

        auto invalidSource = _source({ "invalid" });
        RuntimeRtaskCapabilityRegistry invalid{ _configuration(), invalidSource };
        VERIFY_IS_TRUE(
            invalid.IssueOrRenew(
                TaskA,
                RunA,
                _methods(RtaskMethod::Result),
                100,
                1).Status == RuntimeRtaskCapabilityStatus::TokenSourceFailure);
        auto throwingSource = std::make_shared<FakeRuntimeRtaskCapabilityTokenSource>();
        throwingSource->ThrowOnGenerate = true;
        RuntimeRtaskCapabilityRegistry throwing{ _configuration(), throwingSource };
        VERIFY_IS_TRUE(
            throwing.IssueOrRenew(
                TaskA,
                RunA,
                _methods(RtaskMethod::Result),
                100,
                1).Status == RuntimeRtaskCapabilityStatus::TokenSourceFailure);
    }

    void RuntimeRtaskCapabilityRegistryTests::SystemTokenSourceProducesUniqueValidSecrets()
    {
        SystemRuntimeRtaskCapabilityTokenSource source;
        std::set<std::string> tokens;
        for (size_t index = 0; index < 256; ++index)
        {
            const auto token = source.Generate();
            VERIFY_IS_TRUE(token.has_value());
            if (!token)
            {
                return;
            }
            VERIFY_IS_TRUE(IsValidRtaskCapability(*token));
            VERIFY_IS_TRUE(tokens.emplace(*token).second);
        }
        VERIFY_ARE_EQUAL(size_t{ 256 }, tokens.size());
    }
}
