// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ProtocolHandshake.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        HelloRequest _request()
        {
            HelloRequest request;
            request.ClientInstanceId = "10000000-0000-4000-8000-000000000001";
            request.ClientKind = ProtocolClientKind::TerminalUi;
            request.ExpectedRuntimeIdentity = "terminal-dev:user";
            request.Versions = { 1, 0, 3 };
            request.RequiredCapabilities =
                static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe);
            request.OptionalCapabilities =
                static_cast<uint64_t>(ProtocolCapability::TerminalStream) |
                static_cast<uint64_t>(ProtocolCapability::InputControllerLease);
            request.MaximumAcceptedControlFrameBytes = 512 * 1024;
            return request;
        }

        RuntimeHandshakeConfiguration _runtime()
        {
            RuntimeHandshakeConfiguration runtime;
            runtime.RuntimeIdentity = "terminal-dev:user";
            runtime.RuntimeInstanceId = "20000000-0000-4000-8000-000000000002";
            runtime.Versions = { 1, 1, 2 };
            runtime.Capabilities =
                static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                static_cast<uint64_t>(ProtocolCapability::TaskCommands) |
                static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe) |
                static_cast<uint64_t>(ProtocolCapability::TerminalStream);
            runtime.MaximumControlFrameBytes = 768 * 1024;
            runtime.MaximumTerminalFrameBytes = 128 * 1024;
            runtime.HeartbeatMilliseconds = 5000;
            return runtime;
        }
    }

    class ProtocolHandshakeTests
    {
        TEST_CLASS(ProtocolHandshakeTests);

        TEST_METHOD(NegotiatesOverlappingMinorRangeAndCapabilities);
        TEST_METHOD(RejectsIdentityAndMajorVersionMismatch);
        TEST_METHOD(RejectsNonoverlappingMinorRanges);
        TEST_METHOD(RejectsUnknownOrUnavailableRequiredCapabilities);
        TEST_METHOD(RejectsInvalidClientAndRuntimeBounds);
        TEST_METHOD(ClientValidatesRuntimeResponseIdentityVersionCapabilitiesAndLimits);
    };

    void ProtocolHandshakeTests::NegotiatesOverlappingMinorRangeAndCapabilities()
    {
        const auto decision = NegotiateHandshake(_request(), _runtime());
        VERIFY_IS_TRUE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == HandshakeError::None);
        VERIFY_ARE_EQUAL(uint16_t{ 1 }, decision.Response.ProtocolMajor);
        VERIFY_ARE_EQUAL(uint16_t{ 2 }, decision.Response.ProtocolMinor);
        VERIFY_ARE_EQUAL(uint32_t{ 512 * 1024 }, decision.Response.MaximumControlFrameBytes);
        VERIFY_ARE_EQUAL(uint32_t{ 128 * 1024 }, decision.Response.MaximumTerminalFrameBytes);
        VERIFY_ARE_EQUAL(uint32_t{ 5000 }, decision.Response.HeartbeatMilliseconds);
        VERIFY_IS_TRUE(HasCapability(decision.Response.Capabilities, ProtocolCapability::TaskCommands));
        VERIFY_IS_FALSE(HasCapability(decision.Response.Capabilities, ProtocolCapability::InputControllerLease));
    }

    void ProtocolHandshakeTests::RejectsIdentityAndMajorVersionMismatch()
    {
        auto request = _request();
        request.ExpectedRuntimeIdentity = "terminal-stable:user";
        auto decision = NegotiateHandshake(request, _runtime());
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == HandshakeError::RuntimeIdentityMismatch);

        request = _request();
        request.Versions.Major = 2;
        decision = NegotiateHandshake(request, _runtime());
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == HandshakeError::ProtocolMajorMismatch);
    }

    void ProtocolHandshakeTests::RejectsNonoverlappingMinorRanges()
    {
        auto request = _request();
        request.Versions = { 1, 3, 4 };
        const auto decision = NegotiateHandshake(request, _runtime());
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == HandshakeError::ProtocolMinorMismatch);
    }

    void ProtocolHandshakeTests::RejectsUnknownOrUnavailableRequiredCapabilities()
    {
        auto request = _request();
        request.UnknownRequiredCapabilities.emplace_back("future_required");
        auto decision = NegotiateHandshake(request, _runtime());
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == HandshakeError::MissingRequiredCapability);

        request = _request();
        request.RequiredCapabilities = AddCapability(
            request.RequiredCapabilities,
            ProtocolCapability::RuntimeLifecycle);
        decision = NegotiateHandshake(request, _runtime());
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == HandshakeError::MissingRequiredCapability);

        request = _request();
        request.OptionalCapabilities = static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
        decision = NegotiateHandshake(request, _runtime());
        VERIFY_IS_TRUE(decision.Accepted);
    }

    void ProtocolHandshakeTests::RejectsInvalidClientAndRuntimeBounds()
    {
        auto request = _request();
        request.OptionalCapabilities = request.RequiredCapabilities;
        auto decision = NegotiateHandshake(request, _runtime());
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == HandshakeError::InvalidRequest);

        request = _request();
        request.MaximumAcceptedControlFrameBytes = MinimumControlFrameBytes - 1;
        decision = NegotiateHandshake(request, _runtime());
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == HandshakeError::InvalidRequest);

        auto runtime = _runtime();
        runtime.RuntimeInstanceId = "not-a-uuid";
        decision = NegotiateHandshake(_request(), runtime);
        VERIFY_IS_FALSE(decision.Accepted);
        VERIFY_IS_TRUE(decision.Error == HandshakeError::InvalidRuntimeConfiguration);
    }

    void ProtocolHandshakeTests::ClientValidatesRuntimeResponseIdentityVersionCapabilitiesAndLimits()
    {
        const auto request = _request();
        auto response = NegotiateHandshake(request, _runtime()).Response;
        VERIFY_IS_TRUE(ValidateHelloResponse(request, response).Accepted);

        response.RuntimeIdentity = "other-runtime:user";
        VERIFY_IS_TRUE(ValidateHelloResponse(request, response).Error == HandshakeError::RuntimeIdentityMismatch);

        response = NegotiateHandshake(request, _runtime()).Response;
        response.ProtocolMinor = request.Versions.PreferredMinor + 1;
        VERIFY_IS_TRUE(ValidateHelloResponse(request, response).Error == HandshakeError::ProtocolMinorMismatch);

        response = NegotiateHandshake(request, _runtime()).Response;
        response.Capabilities &= ~static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe);
        VERIFY_IS_TRUE(ValidateHelloResponse(request, response).Error == HandshakeError::MissingRequiredCapability);

        response = NegotiateHandshake(request, _runtime()).Response;
        response.MaximumControlFrameBytes = request.MaximumAcceptedControlFrameBytes + 1;
        VERIFY_IS_TRUE(ValidateHelloResponse(request, response).Error == HandshakeError::InvalidRuntimeConfiguration);
    }
}
