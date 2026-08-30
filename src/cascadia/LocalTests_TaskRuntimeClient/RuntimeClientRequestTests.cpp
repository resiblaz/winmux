// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeClientRequests.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeClientLocalTests
{
    namespace
    {
        constexpr std::array<std::string_view, 20> RequestIds{
            "30000000-0000-4000-8000-000000000101",
            "30000000-0000-4000-8000-000000000102",
            "30000000-0000-4000-8000-000000000103",
            "30000000-0000-4000-8000-000000000104",
            "30000000-0000-4000-8000-000000000105",
            "30000000-0000-4000-8000-000000000106",
            "30000000-0000-4000-8000-000000000107",
            "30000000-0000-4000-8000-000000000108",
            "30000000-0000-4000-8000-000000000109",
            "30000000-0000-4000-8000-000000000110",
            "30000000-0000-4000-8000-000000000111",
            "30000000-0000-4000-8000-000000000112",
            "30000000-0000-4000-8000-000000000113",
            "30000000-0000-4000-8000-000000000114",
            "30000000-0000-4000-8000-000000000115",
            "30000000-0000-4000-8000-000000000116",
            "30000000-0000-4000-8000-000000000117",
            "30000000-0000-4000-8000-000000000118",
            "30000000-0000-4000-8000-000000000119",
            "30000000-0000-4000-8000-000000000120",
        };

        RuntimeClientRequestAttemptInfo Attempt(
            const RuntimeClientOperationId operationId,
            const RuntimeClientAttempt attempt,
            const std::string_view requestId,
            const ControlMethod method = ControlMethod::Ping)
        {
            RuntimeClientRequestAttemptInfo value;
            value.OperationId = operationId;
            value.Attempt = attempt;
            value.RequestId = requestId;
            value.Method = method;
            return value;
        }
    }

    class RuntimeClientRequestTests
    {
        TEST_CLASS(RuntimeClientRequestTests);

        TEST_METHOD(RejectsInvalidDuplicateAndOverCapacityAttempts);
        TEST_METHOD(ResponseMayWinBeforeWriteCompletionReturns);
        TEST_METHOD(ClassifiesStaleUnknownAndLateResponses);
        TEST_METHOD(RetiresWriteStartedMethodsWithExplicitPolicy);
        TEST_METHOD(RetiresWriteCompletedMethodsWithExplicitPolicy);
        TEST_METHOD(QueuedAttemptsSurviveEpochRetirementAndCanBeCancelled);
        TEST_METHOD(RetryRequiresAFreshRequestId);
        TEST_METHOD(SoftTombstonesAreBoundedAndEpochClearable);
        TEST_METHOD(ClearingOneEpochPreservesOtherTombstones);
        TEST_METHOD(StopDrainRemovesOnlyQueuedAttempts);
        TEST_METHOD(RetiresIntoPreallocatedBufferWithoutPartialMutation);
        TEST_METHOD(SupersedesOnlyMatchingActiveEpochAttempts);
    };

    void RuntimeClientRequestTests::RejectsInvalidDuplicateAndOverCapacityAttempts()
    {
        RuntimeClientRequestRegistry invalid{ { 0, 4 } };
        VERIFY_IS_FALSE(invalid.Valid());
        RuntimeClientRequestRegistry noTombstones{ { 1, 0 } };
        VERIFY_IS_FALSE(noTombstones.Valid());
        RuntimeClientRequestRegistry tooManyActive{ { 4097, 1 } };
        VERIFY_IS_FALSE(tooManyActive.Valid());
        RuntimeClientRequestRegistry tooManyTombstones{ { 1, 65537 } };
        VERIFY_IS_FALSE(tooManyTombstones.Valid());

        RuntimeClientRequestRegistry registry{ { 2, 4 } };
        VERIFY_IS_TRUE(registry.Valid());
        auto value = Attempt(0, 1, RequestIds[0]);
        VERIFY_IS_TRUE(registry.Add(value) == RuntimeClientRequestAddStatus::Invalid);
        value = Attempt(1, 0, RequestIds[0]);
        VERIFY_IS_TRUE(registry.Add(value) == RuntimeClientRequestAddStatus::Invalid);
        value = Attempt(1, 1, "not-a-uuid");
        VERIFY_IS_TRUE(registry.Add(value) == RuntimeClientRequestAddStatus::Invalid);
        value = Attempt(1, 1, RequestIds[0], ControlMethod::Count);
        VERIFY_IS_TRUE(registry.Add(value) == RuntimeClientRequestAddStatus::Invalid);
        value = Attempt(1, 1, RequestIds[0], static_cast<ControlMethod>(255));
        VERIFY_IS_TRUE(registry.Add(value) == RuntimeClientRequestAddStatus::Invalid);
        value = Attempt(1, 1, RequestIds[0]);
        value.Transmission = static_cast<ClientRequestTransmission>(255);
        VERIFY_IS_TRUE(registry.Add(value) == RuntimeClientRequestAddStatus::Invalid);
        value = Attempt(1, 1, RequestIds[0]);
        value.Epoch = 1;
        VERIFY_IS_TRUE(registry.Add(value) == RuntimeClientRequestAddStatus::Invalid);

        VERIFY_IS_TRUE(registry.Add(Attempt(1, 1, RequestIds[0])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.Add(Attempt(2, 1, RequestIds[0])) ==
                       RuntimeClientRequestAddStatus::DuplicateRequestId);
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 2, RequestIds[1])) ==
                       RuntimeClientRequestAddStatus::OperationAlreadyActive);
        VERIFY_IS_TRUE(registry.Add(Attempt(2, 1, RequestIds[1])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.Add(Attempt(3, 1, RequestIds[2])) ==
                       RuntimeClientRequestAddStatus::CapacityExceeded);
    }

    void RuntimeClientRequestTests::ResponseMayWinBeforeWriteCompletionReturns()
    {
        RuntimeClientRequestRegistry registry{ { 4, 8 } };
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 1, RequestIds[0])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_FALSE(registry.MarkWriteCompleted(RequestIds[0], 1));
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[0], 1));
        VERIFY_IS_FALSE(registry.BeginWrite(RequestIds[0], 1));

        const auto response = registry.CompleteResponse(1, 1, RequestIds[0]);
        VERIFY_IS_TRUE(response.Status == RuntimeClientResponseMatch::Matched);
        VERIFY_IS_TRUE(response.Attempt.has_value());
        VERIFY_IS_TRUE(response.Attempt->Transmission == ClientRequestTransmission::WriteStarted);
        VERIFY_IS_FALSE(registry.MarkWriteCompleted(RequestIds[0], 1));
        VERIFY_IS_TRUE(registry.CompleteResponse(1, 1, RequestIds[0]).Status ==
                       RuntimeClientResponseMatch::DuplicateOrLate);
    }

    void RuntimeClientRequestTests::ClassifiesStaleUnknownAndLateResponses()
    {
        RuntimeClientRequestRegistry registry{ { 4, 8 } };
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 1, RequestIds[0])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[0], 2));
        VERIFY_IS_TRUE(registry.CompleteResponse(1, 2, RequestIds[0]).Status ==
                       RuntimeClientResponseMatch::StaleEpoch);
        VERIFY_IS_TRUE(registry.CompleteResponse(2, 2, RequestIds[1]).Status ==
                       RuntimeClientResponseMatch::UnknownRequest);
        VERIFY_IS_TRUE(registry.CompleteResponse(2, 2, RequestIds[0]).Status ==
                       RuntimeClientResponseMatch::Matched);
        VERIFY_IS_TRUE(registry.CompleteResponse(2, 2, RequestIds[0]).Status ==
                       RuntimeClientResponseMatch::DuplicateOrLate);
        VERIFY_IS_TRUE(registry.CompleteResponse(0, 2, RequestIds[0]).Status ==
                       RuntimeClientResponseMatch::Invalid);
    }

    void RuntimeClientRequestTests::RetiresWriteStartedMethodsWithExplicitPolicy()
    {
        constexpr std::array methods{
            ControlMethod::Ping,
            ControlMethod::SnapshotAndSubscribe,
            ControlMethod::Unsubscribe,
            ControlMethod::QueryTask,
            ControlMethod::SubmitCommand,
            ControlMethod::AcquireInputLease,
            ControlMethod::ReleaseInputLease,
            ControlMethod::RuntimeStatus,
        };
        constexpr std::array expected{
            ClientDisconnectAction::RetryWithNewRequestId,
            ClientDisconnectAction::Resnapshot,
            ClientDisconnectAction::FailDisconnected,
            ClientDisconnectAction::RetryWithNewRequestId,
            ClientDisconnectAction::OutcomeUnknown,
            ClientDisconnectAction::OutcomeUnknown,
            ClientDisconnectAction::OutcomeUnknown,
            ClientDisconnectAction::RetryWithNewRequestId,
        };
        RuntimeClientRequestRegistry registry{ { methods.size(), 32 } };
        for (size_t index = 0; index < methods.size(); ++index)
        {
            VERIFY_IS_TRUE(registry.Add(Attempt(index + 1, 1, RequestIds[index], methods[index])) ==
                           RuntimeClientRequestAddStatus::Added);
            VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[index], 4));
        }
        const auto retired = registry.RetireEpoch(4);
        VERIFY_ARE_EQUAL(methods.size(), retired.size());
        for (size_t index = 0; index < retired.size(); ++index)
        {
            VERIFY_IS_TRUE(retired[index].Attempt.Method == methods[index]);
            VERIFY_IS_TRUE(retired[index].Action == expected[index]);
        }
    }

    void RuntimeClientRequestTests::RetiresWriteCompletedMethodsWithExplicitPolicy()
    {
        constexpr std::array methods{
            ControlMethod::Ping,
            ControlMethod::SnapshotAndSubscribe,
            ControlMethod::Unsubscribe,
            ControlMethod::QueryTask,
            ControlMethod::SubmitCommand,
            ControlMethod::AcquireInputLease,
            ControlMethod::ReleaseInputLease,
            ControlMethod::RuntimeStatus,
        };
        RuntimeClientRequestRegistry registry{ { methods.size(), 32 } };
        for (size_t index = 0; index < methods.size(); ++index)
        {
            VERIFY_IS_TRUE(registry.Add(Attempt(index + 1, 1, RequestIds[index], methods[index])) ==
                           RuntimeClientRequestAddStatus::Added);
            VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[index], 5));
            VERIFY_IS_TRUE(registry.MarkWriteCompleted(RequestIds[index], 5));
        }
        const auto retired = registry.RetireEpoch(5);
        VERIFY_ARE_EQUAL(methods.size(), retired.size());
        for (const auto& value : retired)
        {
            VERIFY_IS_TRUE(value.Action == EvaluateClientDisconnect(
                                                   value.Attempt.Method,
                                                   ClientRequestTransmission::WriteCompleted));
            VERIFY_IS_TRUE(value.Attempt.Transmission == ClientRequestTransmission::WriteCompleted);
        }
    }

    void RuntimeClientRequestTests::QueuedAttemptsSurviveEpochRetirementAndCanBeCancelled()
    {
        RuntimeClientRequestRegistry registry{ { 4, 8 } };
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 1, RequestIds[0], ControlMethod::SubmitCommand)) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.RetireEpoch(1).empty());
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.QueuedCount());
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[0], 2));
        const auto retired = registry.RetireEpoch(2);
        VERIFY_ARE_EQUAL(size_t{ 1 }, retired.size());
        VERIFY_IS_TRUE(retired[0].Action == ClientDisconnectAction::OutcomeUnknown);

        VERIFY_IS_TRUE(registry.Add(Attempt(2, 1, RequestIds[1])) ==
                       RuntimeClientRequestAddStatus::Added);
        const auto cancelled = registry.CancelQueued(2);
        VERIFY_IS_TRUE(cancelled.has_value());
        VERIFY_IS_FALSE(registry.CancelQueued(2).has_value());
    }

    void RuntimeClientRequestTests::RetryRequiresAFreshRequestId()
    {
        RuntimeClientRequestRegistry registry{ { 4, 8 } };
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 1, RequestIds[0], ControlMethod::QueryTask)) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[0], 1));
        const auto retired = registry.RetireEpoch(1);
        VERIFY_ARE_EQUAL(size_t{ 1 }, retired.size());
        VERIFY_IS_TRUE(retired[0].Action == ClientDisconnectAction::RetryWithNewRequestId);
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 2, RequestIds[0], ControlMethod::QueryTask)) ==
                       RuntimeClientRequestAddStatus::DuplicateRequestId);
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 2, RequestIds[1], ControlMethod::QueryTask)) ==
                       RuntimeClientRequestAddStatus::Added);
    }

    void RuntimeClientRequestTests::SoftTombstonesAreBoundedAndEpochClearable()
    {
        RuntimeClientRequestRegistry registry{ { 1, 2 } };
        for (size_t index = 0; index < 3; ++index)
        {
            VERIFY_IS_TRUE(registry.Add(Attempt(index + 1, 1, RequestIds[index])) ==
                           RuntimeClientRequestAddStatus::Added);
            VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[index], 7));
            VERIFY_IS_TRUE(registry.CompleteResponse(7, 7, RequestIds[index]).Status ==
                           RuntimeClientResponseMatch::Matched);
        }
        VERIFY_ARE_EQUAL(size_t{ 2 }, registry.TombstoneCount());
        VERIFY_IS_TRUE(registry.CompleteResponse(7, 7, RequestIds[0]).Status ==
                       RuntimeClientResponseMatch::UnknownRequest);
        VERIFY_IS_TRUE(registry.CompleteResponse(7, 7, RequestIds[1]).Status ==
                       RuntimeClientResponseMatch::DuplicateOrLate);
        registry.ClearRetiredEpochTombstones(7);
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.TombstoneCount());
        VERIFY_IS_TRUE(registry.Add(Attempt(4, 1, RequestIds[2])) ==
                       RuntimeClientRequestAddStatus::Added);
    }

    void RuntimeClientRequestTests::StopDrainRemovesOnlyQueuedAttempts()
    {
        RuntimeClientRequestRegistry registry{ { 4, 8 } };
        VERIFY_IS_TRUE(registry.Add(Attempt(2, 1, RequestIds[1])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 1, RequestIds[0])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.Add(Attempt(3, 1, RequestIds[2])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[2], 9));

        const auto drained = registry.DrainQueuedForStop();
        VERIFY_ARE_EQUAL(size_t{ 2 }, drained.size());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, drained[0].OperationId);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, drained[1].OperationId);
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.ActiveCount());
        VERIFY_IS_FALSE(registry.CancelQueued(3).has_value());
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.RetireEpoch(9).size());
    }

    void RuntimeClientRequestTests::ClearingOneEpochPreservesOtherTombstones()
    {
        RuntimeClientRequestRegistry registry{ { 2, 4 } };
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 1, RequestIds[0])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[0], 1));
        VERIFY_IS_TRUE(registry.CompleteResponse(1, 1, RequestIds[0]).Status ==
                       RuntimeClientResponseMatch::Matched);

        VERIFY_IS_TRUE(registry.Add(Attempt(2, 1, RequestIds[1])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[1], 2));
        VERIFY_IS_TRUE(registry.CompleteResponse(2, 2, RequestIds[1]).Status ==
                       RuntimeClientResponseMatch::Matched);
        VERIFY_IS_TRUE(registry.Add(Attempt(3, 1, RequestIds[2])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.CancelQueued(3).has_value());
        VERIFY_ARE_EQUAL(size_t{ 3 }, registry.TombstoneCount());

        registry.ClearRetiredEpochTombstones(1);
        VERIFY_ARE_EQUAL(size_t{ 2 }, registry.TombstoneCount());
        VERIFY_IS_TRUE(registry.CompleteResponse(1, 1, RequestIds[0]).Status ==
                       RuntimeClientResponseMatch::UnknownRequest);
        VERIFY_IS_TRUE(registry.CompleteResponse(2, 2, RequestIds[1]).Status ==
                       RuntimeClientResponseMatch::DuplicateOrLate);
        VERIFY_IS_TRUE(registry.Add(Attempt(4, 1, RequestIds[2])) ==
                       RuntimeClientRequestAddStatus::DuplicateRequestId);

        registry.ClearRetiredEpochTombstones(2);
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.TombstoneCount());
    }

    void RuntimeClientRequestTests::RetiresIntoPreallocatedBufferWithoutPartialMutation()
    {
        RuntimeClientRequestRegistry registry{ { 2, 4 } };
        VERIFY_IS_TRUE(registry.Add(Attempt(2, 1, RequestIds[1], ControlMethod::SubmitCommand)) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.Add(Attempt(1, 1, RequestIds[0], ControlMethod::QueryTask)) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[0], 3));
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[1], 3));

        std::vector<RuntimeClientRetiredRequest> retired;
        VERIFY_IS_FALSE(registry.RetireEpochInto(3, retired));
        VERIFY_ARE_EQUAL(size_t{ 2 }, registry.ActiveCount());
        retired.reserve(2);
        VERIFY_IS_TRUE(registry.RetireEpochInto(3, retired));
        VERIFY_ARE_EQUAL(size_t{ 2 }, retired.size());
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, retired[0].Attempt.OperationId);
        VERIFY_ARE_EQUAL(uint64_t{ 2 }, retired[1].Attempt.OperationId);
        VERIFY_IS_TRUE(retired[0].Action == ClientDisconnectAction::RetryWithNewRequestId);
        VERIFY_IS_TRUE(retired[1].Action == ClientDisconnectAction::OutcomeUnknown);
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.ActiveCount());
    }

    void RuntimeClientRequestTests::SupersedesOnlyMatchingActiveEpochAttempts()
    {
        RuntimeClientRequestRegistry registry{ { 2, 1 } };
        VERIFY_IS_TRUE(registry.SupersedeActive(0, 7) ==
                       RuntimeClientRequestSupersedeStatus::Invalid);
        VERIFY_IS_TRUE(registry.SupersedeActive(1, 0) ==
                       RuntimeClientRequestSupersedeStatus::Invalid);
        VERIFY_IS_TRUE(registry.SupersedeActive(99, 7) ==
                       RuntimeClientRequestSupersedeStatus::NotFound);

        VERIFY_IS_TRUE(registry.Add(Attempt(1, 1, RequestIds[0])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.SupersedeActive(1, 7) ==
                       RuntimeClientRequestSupersedeStatus::WrongTransmission);
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[0], 7));
        VERIFY_IS_TRUE(registry.SupersedeActive(1, 8) ==
                       RuntimeClientRequestSupersedeStatus::Stale);
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.ActiveCount());
        VERIFY_IS_TRUE(registry.SupersedeActive(1, 7) ==
                       RuntimeClientRequestSupersedeStatus::Superseded);
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.ActiveCount());
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.TombstoneCount());
        VERIFY_IS_TRUE(registry.CompleteResponse(7, 7, RequestIds[0]).Status ==
                       RuntimeClientResponseMatch::DuplicateOrLate);
        VERIFY_IS_TRUE(registry.SupersedeActive(1, 7) ==
                       RuntimeClientRequestSupersedeStatus::NotFound);

        VERIFY_IS_TRUE(registry.Add(Attempt(2, 1, RequestIds[1])) ==
                       RuntimeClientRequestAddStatus::Added);
        VERIFY_IS_TRUE(registry.BeginWrite(RequestIds[1], 7));
        VERIFY_IS_TRUE(registry.MarkWriteCompleted(RequestIds[1], 7));
        VERIFY_IS_TRUE(registry.SupersedeActive(2, 7) ==
                       RuntimeClientRequestSupersedeStatus::Superseded);
        VERIFY_ARE_EQUAL(size_t{ 0 }, registry.ActiveCount());
    }
}
