// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ProtocolJson.h"

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
            request.Versions = { 1, 0, 2 };
            request.RequiredCapabilities = static_cast<uint64_t>(ProtocolCapability::SnapshotAndSubscribe);
            request.OptionalCapabilities = static_cast<uint64_t>(ProtocolCapability::TerminalStream);
            request.MaximumAcceptedControlFrameBytes = 512 * 1024;
            return request;
        }

        HelloResponse _response()
        {
            HelloResponse response;
            response.RuntimeIdentity = "terminal-dev:user";
            response.RuntimeInstanceId = "20000000-0000-4000-8000-000000000002";
            response.ProtocolMajor = 1;
            response.ProtocolMinor = 2;
            response.Capabilities =
                static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                static_cast<uint64_t>(ProtocolCapability::TerminalStream);
            response.MaximumControlFrameBytes = 512 * 1024;
            response.MaximumTerminalFrameBytes = 128 * 1024;
            response.HeartbeatMilliseconds = 5000;
            return response;
        }
    }

    class ProtocolJsonTests
    {
        TEST_CLASS(ProtocolJsonTests);

        TEST_METHOD(HelloRequestRoundTripsCanonicalSchema);
        TEST_METHOD(UnknownOptionalCapabilityIsIgnoredButRequiredIsRetained);
        TEST_METHOD(RejectsDuplicateUnknownAndTrailingJsonFields);
        TEST_METHOD(HelloResponseRoundTripsAndIgnoresFutureCapability);
        TEST_METHOD(RejectsInvalidUtf8OversizeAndInvalidBounds);
    };

    void ProtocolJsonTests::HelloRequestRoundTripsCanonicalSchema()
    {
        const auto encoded = EncodeHelloRequest(_request());
        VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_IS_TRUE(encoded.Payload.find("\"type\":\"hello\"") != std::string::npos);

        const auto decoded = DecodeHelloRequest(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(_request().ClientInstanceId, decoded.Value.ClientInstanceId);
        VERIFY_IS_TRUE(decoded.Value.ClientKind == ProtocolClientKind::TerminalUi);
        VERIFY_ARE_EQUAL(_request().ExpectedRuntimeIdentity, decoded.Value.ExpectedRuntimeIdentity);
        VERIFY_ARE_EQUAL(uint16_t{ 2 }, decoded.Value.Versions.PreferredMinor);
        VERIFY_IS_TRUE(HasCapability(decoded.Value.RequiredCapabilities, ProtocolCapability::SnapshotAndSubscribe));
        VERIFY_IS_TRUE(HasCapability(decoded.Value.OptionalCapabilities, ProtocolCapability::TerminalStream));
        VERIFY_ARE_EQUAL(uint32_t{ 512 * 1024 }, decoded.Value.MaximumAcceptedControlFrameBytes);
    }

    void ProtocolJsonTests::UnknownOptionalCapabilityIsIgnoredButRequiredIsRetained()
    {
        constexpr std::string_view optionalFuture = R"JSON({
"type":"hello","clientInstanceId":"10000000-0000-4000-8000-000000000001",
"clientKind":"terminal_ui","expectedRuntimeIdentity":"terminal-dev:user",
"protocol":{"major":1,"minimumMinor":0,"preferredMinor":2},
"requiredCapabilities":["snapshot_and_subscribe"],
"optionalCapabilities":["future_optional"],"maxControlFrameBytes":524288})JSON";
        auto decoded = DecodeHelloRequest(optionalFuture);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 0 }, decoded.Value.UnknownRequiredCapabilities.size());
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, decoded.Value.OptionalCapabilities);

        constexpr std::string_view requiredFuture = R"JSON({
"type":"hello","clientInstanceId":"10000000-0000-4000-8000-000000000001",
"clientKind":"terminal_ui","expectedRuntimeIdentity":"terminal-dev:user",
"protocol":{"major":1,"minimumMinor":0,"preferredMinor":2},
"requiredCapabilities":["future_required"],
"optionalCapabilities":[],"maxControlFrameBytes":524288})JSON";
        decoded = DecodeHelloRequest(requiredFuture);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, decoded.Value.UnknownRequiredCapabilities.size());
        VERIFY_ARE_EQUAL(std::string{ "future_required" }, decoded.Value.UnknownRequiredCapabilities[0]);

        RuntimeHandshakeConfiguration runtime;
        runtime.RuntimeIdentity = "terminal-dev:user";
        runtime.RuntimeInstanceId = "20000000-0000-4000-8000-000000000002";
        runtime.Capabilities = AllProtocolCapabilities;
        VERIFY_IS_TRUE(NegotiateHandshake(decoded.Value, runtime).Error == HandshakeError::MissingRequiredCapability);
    }

    void ProtocolJsonTests::RejectsDuplicateUnknownAndTrailingJsonFields()
    {
        constexpr std::string_view duplicate = R"JSON({
"type":"hello","type":"hello","clientInstanceId":"10000000-0000-4000-8000-000000000001",
"clientKind":"terminal_ui","expectedRuntimeIdentity":"terminal-dev:user",
"protocol":{"major":1,"minimumMinor":0,"preferredMinor":0},
"requiredCapabilities":[],"optionalCapabilities":[],"maxControlFrameBytes":524288})JSON";
        VERIFY_IS_TRUE(DecodeHelloRequest(duplicate).Status == ProtocolJsonStatus::MalformedJson);

        const auto encoded = EncodeHelloRequest(_request());
        auto unknownField = encoded.Payload;
        unknownField.insert(unknownField.size() - 1, ",\"unexpected\":true");
        VERIFY_IS_TRUE(DecodeHelloRequest(unknownField).Status == ProtocolJsonStatus::InvalidSchema);

        auto trailing = encoded.Payload;
        trailing.append(" trailing");
        VERIFY_IS_TRUE(DecodeHelloRequest(trailing).Status == ProtocolJsonStatus::MalformedJson);

        auto duplicateCapability = encoded.Payload;
        const auto capability = std::string{ "\"terminal_stream\"" };
        const auto position = duplicateCapability.find(capability);
        duplicateCapability.insert(position + capability.size(), ",\"terminal_stream\"");
        VERIFY_IS_TRUE(DecodeHelloRequest(duplicateCapability).Status == ProtocolJsonStatus::InvalidSchema);
    }

    void ProtocolJsonTests::HelloResponseRoundTripsAndIgnoresFutureCapability()
    {
        const auto encoded = EncodeHelloResponse(_response());
        VERIFY_IS_TRUE(encoded.Succeeded);
        auto decoded = DecodeHelloResponse(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(_response().RuntimeInstanceId, decoded.Value.RuntimeInstanceId);
        VERIFY_ARE_EQUAL(uint16_t{ 2 }, decoded.Value.ProtocolMinor);
        VERIFY_IS_TRUE(HasCapability(decoded.Value.Capabilities, ProtocolCapability::TaskQueries));

        auto future = encoded.Payload;
        const auto capability = std::string{ "\"terminal_stream\"" };
        const auto position = future.find(capability);
        future.insert(position + capability.size(), ",\"future_server_capability\"");
        decoded = DecodeHelloResponse(future);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_IS_TRUE(HasCapability(decoded.Value.Capabilities, ProtocolCapability::TerminalStream));
    }

    void ProtocolJsonTests::RejectsInvalidUtf8OversizeAndInvalidBounds()
    {
        const std::string invalidUtf8{ "\xc0\x80", 2 };
        VERIFY_IS_TRUE(DecodeHelloRequest(invalidUtf8).Status == ProtocolJsonStatus::InvalidUtf8);

        const std::string oversized(MaximumControlFrameBytes + 1, 'x');
        VERIFY_IS_TRUE(DecodeHelloRequest(oversized).Status == ProtocolJsonStatus::Oversized);

        auto invalid = _request();
        invalid.MaximumAcceptedControlFrameBytes = MinimumControlFrameBytes - 1;
        VERIFY_IS_FALSE(EncodeHelloRequest(invalid).Succeeded);

        auto invalidResponse = _response();
        invalidResponse.HeartbeatMilliseconds = 100;
        VERIFY_IS_FALSE(EncodeHelloResponse(invalidResponse).Succeeded);
    }
}
