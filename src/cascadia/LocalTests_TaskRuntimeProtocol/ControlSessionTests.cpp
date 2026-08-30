// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ControlSession.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        RuntimeHandshakeConfiguration _runtime()
        {
            RuntimeHandshakeConfiguration runtime;
            runtime.RuntimeIdentity = "terminal-dev:user";
            runtime.RuntimeInstanceId = "20000000-0000-4000-8000-000000000002";
            runtime.Capabilities =
                static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe);
            return runtime;
        }

        HelloRequest _hello()
        {
            HelloRequest request;
            request.ClientInstanceId = "10000000-0000-4000-8000-000000000001";
            request.ExpectedRuntimeIdentity = "terminal-dev:user";
            request.RequiredCapabilities = static_cast<uint64_t>(ProtocolCapability::TaskQueries);
            request.OptionalCapabilities = static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe);
            return request;
        }

        ControlRequest _request(const uint64_t suffix, const ControlMethod method)
        {
            auto id = std::string{ "30000000-0000-4000-8000-000000000000" };
            constexpr std::string_view digits{ "0123456789abcdef" };
            id.back() = digits[suffix & 0xf];
            return { std::move(id), method, "{}" };
        }
    }

    class ControlSessionTests
    {
        TEST_CLASS(ControlSessionTests);

        TEST_METHOD(RequiresHelloBeforeAnyRequest);
        TEST_METHOD(AcceptedHelloEnablesNegotiatedMethods);
        TEST_METHOD(MissingCapabilityRejectsMethodWithoutClosingSession);
        TEST_METHOD(DuplicateHelloMovesSessionToClosing);
        TEST_METHOD(BoundsAndCorrelatesInflightRequests);
        TEST_METHOD(ClosingAndClosedSessionsRejectNewWork);
    };

    void ControlSessionTests::RequiresHelloBeforeAnyRequest()
    {
        ControlSession session{ _runtime() };
        const auto decision = session.BeginRequest(_request(1, ControlMethod::Ping));
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == ProtocolRpcErrorCode::HandshakeRequired);
        VERIFY_IS_TRUE(session.State() == ControlSessionState::AwaitingHello);
    }

    void ControlSessionTests::AcceptedHelloEnablesNegotiatedMethods()
    {
        ControlSession session{ _runtime() };
        const auto hello = session.AcceptHello(_hello());
        VERIFY_IS_TRUE(hello.Accepted);
        VERIFY_IS_TRUE(session.State() == ControlSessionState::Ready);
        VERIFY_IS_TRUE(HasCapability(session.NegotiatedCapabilities(), ProtocolCapability::TaskQueries));

        const auto query = _request(1, ControlMethod::QueryTask);
        VERIFY_IS_TRUE(session.BeginRequest(query).Accepted);
        VERIFY_ARE_EQUAL(size_t{ 1 }, session.InflightRequestCount());
        VERIFY_IS_TRUE(session.CompleteRequest(query.RequestId));
        VERIFY_ARE_EQUAL(size_t{ 0 }, session.InflightRequestCount());
    }

    void ControlSessionTests::MissingCapabilityRejectsMethodWithoutClosingSession()
    {
        ControlSession session{ _runtime() };
        VERIFY_IS_TRUE(session.AcceptHello(_hello()).Accepted);
        const auto decision = session.BeginRequest(_request(1, ControlMethod::SubmitCommand));
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == ProtocolRpcErrorCode::CapabilityRequired);
        VERIFY_IS_TRUE(session.State() == ControlSessionState::Ready);
        VERIFY_ARE_EQUAL(size_t{ 0 }, session.InflightRequestCount());
    }

    void ControlSessionTests::DuplicateHelloMovesSessionToClosing()
    {
        ControlSession session{ _runtime() };
        VERIFY_IS_TRUE(session.AcceptHello(_hello()).Accepted);
        const auto duplicate = session.AcceptHello(_hello());
        VERIFY_IS_FALSE(duplicate.Accepted);
        VERIFY_IS_TRUE(duplicate.Error == HandshakeError::InvalidRequest);
        VERIFY_IS_TRUE(session.State() == ControlSessionState::Closing);
    }

    void ControlSessionTests::BoundsAndCorrelatesInflightRequests()
    {
        ControlSession session{ _runtime(), 2 };
        VERIFY_IS_TRUE(session.AcceptHello(_hello()).Accepted);
        const auto first = _request(1, ControlMethod::Ping);
        const auto second = _request(2, ControlMethod::QueryTask);
        VERIFY_IS_TRUE(session.BeginRequest(first).Accepted);
        const auto duplicate = session.BeginRequest(first);
        VERIFY_IS_TRUE(duplicate.Error == ProtocolRpcErrorCode::DuplicateRequest);
        VERIFY_IS_TRUE(session.BeginRequest(second).Accepted);
        const auto overflow = session.BeginRequest(_request(3, ControlMethod::Ping));
        VERIFY_IS_TRUE(overflow.Error == ProtocolRpcErrorCode::TooManyRequests);
        VERIFY_IS_FALSE(session.CompleteRequest("not-a-uuid"));
        VERIFY_IS_TRUE(session.CompleteRequest(first.RequestId));
        VERIFY_IS_FALSE(session.CompleteRequest(first.RequestId));
        VERIFY_IS_TRUE(session.BeginRequest(_request(3, ControlMethod::Ping)).Accepted);
    }

    void ControlSessionTests::ClosingAndClosedSessionsRejectNewWork()
    {
        ControlSession session{ _runtime() };
        VERIFY_IS_TRUE(session.AcceptHello(_hello()).Accepted);
        session.BeginClose();
        auto decision = session.BeginRequest(_request(1, ControlMethod::Ping));
        VERIFY_IS_TRUE(decision.Error == ProtocolRpcErrorCode::RuntimeUnavailable);
        session.MarkClosed();
        VERIFY_IS_TRUE(session.State() == ControlSessionState::Closed);
        decision = session.BeginRequest(_request(2, ControlMethod::Ping));
        VERIFY_IS_TRUE(decision.Error == ProtocolRpcErrorCode::RuntimeUnavailable);
    }
}
