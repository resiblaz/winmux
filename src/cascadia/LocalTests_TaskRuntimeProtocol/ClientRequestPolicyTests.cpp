// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ClientRequestPolicy.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    class ClientRequestPolicyTests
    {
        TEST_CLASS(ClientRequestPolicyTests);

        TEST_METHOD(ReadOnlyRequestsRetryWithANewRequestId);
        TEST_METHOD(SubscriptionMethodsUseExplicitReconnectActions);
        TEST_METHOD(MutationsBecomeOutcomeUnknownAfterWriteStarts);
    };

    void ClientRequestPolicyTests::ReadOnlyRequestsRetryWithANewRequestId()
    {
        for (const auto method : { ControlMethod::Ping, ControlMethod::QueryTask, ControlMethod::RuntimeStatus })
        {
            VERIFY_IS_TRUE(IsReadOnlyControlMethod(method));
            for (const auto transmission : {
                     ClientRequestTransmission::NotSent,
                     ClientRequestTransmission::WriteStarted,
                     ClientRequestTransmission::WriteCompleted })
            {
                VERIFY_IS_TRUE(EvaluateClientDisconnect(method, transmission) ==
                               ClientDisconnectAction::RetryWithNewRequestId);
            }
        }
    }

    void ClientRequestPolicyTests::SubscriptionMethodsUseExplicitReconnectActions()
    {
        for (const auto transmission : {
                 ClientRequestTransmission::NotSent,
                 ClientRequestTransmission::WriteStarted,
                 ClientRequestTransmission::WriteCompleted })
        {
            VERIFY_IS_TRUE(EvaluateClientDisconnect(ControlMethod::SnapshotAndSubscribe, transmission) ==
                           ClientDisconnectAction::Resnapshot);
            VERIFY_IS_TRUE(EvaluateClientDisconnect(ControlMethod::Unsubscribe, transmission) ==
                           ClientDisconnectAction::FailDisconnected);
        }
    }

    void ClientRequestPolicyTests::MutationsBecomeOutcomeUnknownAfterWriteStarts()
    {
        for (const auto method : {
                 ControlMethod::SubmitCommand,
                 ControlMethod::AcquireInputLease,
                 ControlMethod::ReleaseInputLease })
        {
            VERIFY_IS_FALSE(IsReadOnlyControlMethod(method));
            VERIFY_IS_TRUE(EvaluateClientDisconnect(method, ClientRequestTransmission::NotSent) ==
                           ClientDisconnectAction::RetryWithNewRequestId);
            VERIFY_IS_TRUE(EvaluateClientDisconnect(method, ClientRequestTransmission::WriteStarted) ==
                           ClientDisconnectAction::OutcomeUnknown);
            VERIFY_IS_TRUE(EvaluateClientDisconnect(method, ClientRequestTransmission::WriteCompleted) ==
                           ClientDisconnectAction::OutcomeUnknown);
        }
        VERIFY_IS_FALSE(IsReadOnlyControlMethod(ControlMethod::Count));
        VERIFY_IS_TRUE(EvaluateClientDisconnect(
                           ControlMethod::Count,
                           ClientRequestTransmission::NotSent) ==
                       ClientDisconnectAction::OutcomeUnknown);
        VERIFY_IS_TRUE(EvaluateClientDisconnect(
                           ControlMethod::SubmitCommand,
                           static_cast<ClientRequestTransmission>(255)) ==
                       ClientDisconnectAction::OutcomeUnknown);
        const auto unknownMethod = static_cast<ControlMethod>(255);
        VERIFY_IS_FALSE(IsReadOnlyControlMethod(unknownMethod));
        VERIFY_IS_TRUE(EvaluateClientDisconnect(
                           unknownMethod,
                           ClientRequestTransmission::NotSent) ==
                       ClientDisconnectAction::OutcomeUnknown);
    }
}
