// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeTerminalClientRegistry.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr uint64_t Generation{ 7 };
        constexpr std::string_view ClientA{
            "98000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view ClientB{
            "98000000-0000-4000-8000-000000000002"
        };
        constexpr std::string_view ClientC{
            "98000000-0000-4000-8000-000000000003"
        };

        TerminalFrame _output(
            const uint64_t sequence,
            std::vector<uint8_t> payload,
            const uint64_t generation = Generation)
        {
            TerminalFrame frame;
            frame.Kind = TerminalFrameKind::Output;
            frame.Sequence = sequence;
            frame.Generation = generation;
            frame.Payload = std::move(payload);
            return frame;
        }

        RuntimeTerminalClientRegistryConfiguration _configuration()
        {
            return {
                Generation,
                2,
                70,
                2,
                10,
            };
        }
    }

    class RuntimeTerminalClientRegistryTests
    {
        TEST_CLASS(RuntimeTerminalClientRegistryTests);

        TEST_METHOD(RejectsInvalidConfigurationAndAttachIdentity);
        TEST_METHOD(AttachIsUniqueAndCapacityBounded);
        TEST_METHOD(PerClientQueueBoundsIsolateSlowPeer);
        TEST_METHOD(QueueManyPreflightDoesNotPartiallyMutateOnBoundsFailure);
        TEST_METHOD(PullPreservesFifoAndExactWireByteAccounting);
        TEST_METHOD(AcknowledgementIsMonotonicGenerationBoundedAndPerClient);
        TEST_METHOD(LeaseBindingAndDetachReturnExactRevision);
    };

    void RuntimeTerminalClientRegistryTests::RejectsInvalidConfigurationAndAttachIdentity()
    {
        const std::array configurations{
            RuntimeTerminalClientRegistryConfiguration{ 0, 1, 1, 1, 1 },
            RuntimeTerminalClientRegistryConfiguration{ 1, 0, 1, 1, 1 },
            RuntimeTerminalClientRegistryConfiguration{ 1, 1, 0, 1, 1 },
            RuntimeTerminalClientRegistryConfiguration{ 1, 1, 1, 0, 1 },
            RuntimeTerminalClientRegistryConfiguration{ 1, 1, 1, 1, 0 },
            RuntimeTerminalClientRegistryConfiguration{
                1,
                1,
                1,
                1,
                MaximumTerminalFrameBytes + 1,
            },
        };
        for (const auto& configuration : configurations)
        {
            RuntimeTerminalClientRegistry registry{ configuration };
            VERIFY_IS_FALSE(registry.Valid());
            VERIFY_IS_TRUE(
                registry.Attach(ClientA, configuration.Generation, 0).Status ==
                RuntimeTerminalClientRegistryStatus::InvalidConfiguration);
        }

        RuntimeTerminalClientRegistry registry{ _configuration() };
        VERIFY_IS_TRUE(
            registry.Attach("not-a-client", Generation, 0).Status ==
            RuntimeTerminalClientRegistryStatus::InvalidClient);
        VERIFY_IS_TRUE(
            registry.Attach(ClientA, Generation + 1, 0).Status ==
            RuntimeTerminalClientRegistryStatus::StaleGeneration);
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.Size());
    }

    void RuntimeTerminalClientRegistryTests::AttachIsUniqueAndCapacityBounded()
    {
        RuntimeTerminalClientRegistry registry{ _configuration() };
        VERIFY_IS_TRUE(
            registry.Attach(ClientA, Generation, 5).Status ==
            RuntimeTerminalClientRegistryStatus::Attached);
        VERIFY_IS_TRUE(
            registry.Attach(ClientA, Generation, 0).Status ==
            RuntimeTerminalClientRegistryStatus::AlreadyAttached);
        VERIFY_IS_TRUE(
            registry.Attach(ClientB, Generation, 7).Status ==
            RuntimeTerminalClientRegistryStatus::Attached);
        VERIFY_IS_TRUE(
            registry.Attach(ClientC, Generation, 0).Status ==
            RuntimeTerminalClientRegistryStatus::CapacityExceeded);
        VERIFY_ARE_EQUAL(size_t{ 2 }, registry.Size());
        const auto clients = registry.ClientIds();
        VERIFY_IS_TRUE((
            clients == std::vector<std::string>{
                std::string{ ClientA },
                std::string{ ClientB },
            }));
        VERIFY_ARE_EQUAL(
            uint64_t{ 5 },
            registry.Find(ClientA)->LastAcknowledgedOutputSequence);
    }

    void RuntimeTerminalClientRegistryTests::PerClientQueueBoundsIsolateSlowPeer()
    {
        RuntimeTerminalClientRegistry registry{ _configuration() };
        (void)registry.Attach(ClientA, Generation, 0);
        (void)registry.Attach(ClientB, Generation, 0);
        VERIFY_IS_TRUE(
            registry.Queue(ClientA, _output(1, { 'a', 'b' })).Status ==
            RuntimeTerminalClientRegistryStatus::Queued);
        VERIFY_IS_TRUE(
            registry.Queue(ClientA, _output(2, { 'c', 'd' })).Status ==
            RuntimeTerminalClientRegistryStatus::Queued);
        VERIFY_IS_TRUE(
            registry.Queue(ClientA, _output(3, { 'e' })).Status ==
            RuntimeTerminalClientRegistryStatus::SlowPeer);

        const auto slow = registry.Find(ClientA);
        const auto observer = registry.Find(ClientB);
        VERIFY_ARE_EQUAL(size_t{ 2 }, slow->QueuedFrames);
        VERIFY_ARE_EQUAL(size_t{ 68 }, slow->QueuedBytes);
        VERIFY_ARE_EQUAL(size_t{ 0 }, observer->QueuedFrames);
        VERIFY_ARE_EQUAL(size_t{ 0 }, observer->QueuedBytes);
        VERIFY_ARE_EQUAL(size_t{ 2 }, registry.Size());
    }

    void RuntimeTerminalClientRegistryTests::QueueManyPreflightDoesNotPartiallyMutateOnBoundsFailure()
    {
        RuntimeTerminalClientRegistry registry{ _configuration() };
        (void)registry.Attach(ClientA, Generation, 0);
        const std::array frames{
            _output(1, { 'a', 'b' }),
            _output(2, { 'c', 'd' }),
            _output(3, { 'e' }),
        };
        VERIFY_IS_TRUE(
            registry.QueueMany(ClientA, frames).Status ==
            RuntimeTerminalClientRegistryStatus::SlowPeer);
        const auto snapshot = registry.Find(ClientA);
        VERIFY_ARE_EQUAL(size_t{ 0 }, snapshot->QueuedFrames);
        VERIFY_ARE_EQUAL(size_t{ 0 }, snapshot->QueuedBytes);

        auto invalid = _output(2, { 'x' }, Generation + 1);
        const std::array invalidBatch{
            _output(1, { 'a' }),
            std::move(invalid),
        };
        VERIFY_IS_TRUE(
            registry.QueueMany(ClientA, invalidBatch).Status ==
            RuntimeTerminalClientRegistryStatus::InvalidFrame);
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.Find(ClientA)->QueuedFrames);
    }

    void RuntimeTerminalClientRegistryTests::PullPreservesFifoAndExactWireByteAccounting()
    {
        RuntimeTerminalClientRegistry registry{ _configuration() };
        (void)registry.Attach(ClientA, Generation, 0);
        (void)registry.Queue(ClientA, _output(4, { 'a', 'b' }));
        (void)registry.Queue(ClientA, _output(9, { 'c' }));
        VERIFY_ARE_EQUAL(size_t{ 67 }, registry.Find(ClientA)->QueuedBytes);

        const auto first = registry.Pull(ClientA);
        VERIFY_IS_TRUE(first.Status == RuntimeTerminalClientRegistryStatus::Pulled);
        VERIFY_ARE_EQUAL(uint64_t{ 4 }, first.Frame->Sequence);
        VERIFY_ARE_EQUAL(size_t{ 33 }, registry.Find(ClientA)->QueuedBytes);
        const auto second = registry.Pull(ClientA);
        VERIFY_ARE_EQUAL(uint64_t{ 9 }, second.Frame->Sequence);
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.Find(ClientA)->QueuedBytes);
        VERIFY_IS_TRUE(
            registry.Pull(ClientA).Status ==
            RuntimeTerminalClientRegistryStatus::Empty);
    }

    void RuntimeTerminalClientRegistryTests::AcknowledgementIsMonotonicGenerationBoundedAndPerClient()
    {
        RuntimeTerminalClientRegistry registry{ _configuration() };
        (void)registry.Attach(ClientA, Generation, 0);
        (void)registry.Attach(ClientB, Generation, 2);
        VERIFY_IS_TRUE(
            registry.Acknowledge(ClientA, Generation, 4, 10).Status ==
            RuntimeTerminalClientRegistryStatus::Acknowledged);
        VERIFY_IS_TRUE(
            registry.Acknowledge(ClientA, Generation, 4, 10).Status ==
            RuntimeTerminalClientRegistryStatus::DuplicateAcknowledgement);
        VERIFY_IS_TRUE(
            registry.Acknowledge(ClientA, Generation, 3, 10).Status ==
            RuntimeTerminalClientRegistryStatus::AcknowledgementRegression);
        VERIFY_IS_TRUE(
            registry.Acknowledge(ClientA, Generation, 11, 10).Status ==
            RuntimeTerminalClientRegistryStatus::AcknowledgementAhead);
        VERIFY_IS_TRUE(
            registry.Acknowledge(ClientA, Generation + 1, 5, 10).Status ==
            RuntimeTerminalClientRegistryStatus::StaleGeneration);
        VERIFY_ARE_EQUAL(
            uint64_t{ 4 },
            registry.Find(ClientA)->LastAcknowledgedOutputSequence);
        VERIFY_ARE_EQUAL(
            uint64_t{ 2 },
            registry.Find(ClientB)->LastAcknowledgedOutputSequence);
    }

    void RuntimeTerminalClientRegistryTests::LeaseBindingAndDetachReturnExactRevision()
    {
        RuntimeTerminalClientRegistry registry{ _configuration() };
        (void)registry.Attach(ClientA, Generation, 0);
        VERIFY_IS_TRUE(
            registry.BindLease(ClientA, 0).Status ==
            RuntimeTerminalClientRegistryStatus::InvalidLeaseRevision);
        VERIFY_IS_TRUE(
            registry.BindLease(ClientA, 42).Status ==
            RuntimeTerminalClientRegistryStatus::LeaseBound);
        VERIFY_IS_TRUE(registry.Find(ClientA)->LeaseRevision == std::optional<uint64_t>{ 42 });
        const auto cleared = registry.ClearLease(ClientA);
        VERIFY_IS_TRUE(cleared.Status == RuntimeTerminalClientRegistryStatus::LeaseCleared);
        VERIFY_IS_TRUE(cleared.LeaseRevision == std::optional<uint64_t>{ 42 });
        VERIFY_IS_FALSE(registry.Find(ClientA)->LeaseRevision.has_value());

        (void)registry.BindLease(ClientA, 43);
        const auto detached = registry.Detach(ClientA);
        VERIFY_IS_TRUE(detached.Status == RuntimeTerminalClientRegistryStatus::Detached);
        VERIFY_IS_TRUE(detached.LeaseRevision == std::optional<uint64_t>{ 43 });
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.Size());
        VERIFY_IS_TRUE(
            registry.Detach(ClientA).Status ==
            RuntimeTerminalClientRegistryStatus::NotFound);
    }
}
