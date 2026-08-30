// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RtaskCommandJson.h"

#include "ProtocolVersion.h"

#include <limits>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        constexpr std::string_view CommandId{
            "a3000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view RunId{
            "a4000000-0000-4000-8000-000000000001"
        };

        RtaskCommandRequest _request(const RtaskMethod method, std::string text = {})
        {
            return {
                std::string{ CommandId },
                std::string{ RunId },
                std::string(RtaskCapabilityBytes, 'b'),
                method,
                std::move(text),
            };
        }
    }

    class RtaskCommandJsonTests
    {
        TEST_CLASS(RtaskCommandJsonTests);

        TEST_METHOD(FourRequestsRoundTripWithMethodExactSchemas);
        TEST_METHOD(CommandResultRoundTripsUnsignedCursors);
        TEST_METHOD(RepeatedEncodingPreservesIdempotencyIdentity);
        TEST_METHOD(RejectsUnknownExtraDuplicateAndNumericDrift);
        TEST_METHOD(MalformedAndOversizedErrorsNeverEchoSecrets);
    };

    void RtaskCommandJsonTests::FourRequestsRoundTripWithMethodExactSchemas()
    {
        const std::array requests{
            _request(RtaskMethod::StatusInProgress),
            _request(RtaskMethod::StatusInReview),
            _request(RtaskMethod::StatusBlocked, "blocked reason"),
            _request(RtaskMethod::Result, "task result"),
        };
        for (const auto& request : requests)
        {
            const auto encoded = EncodeRtaskCommandRequest(request);
            VERIFY_IS_TRUE(encoded.Succeeded);
            const auto decoded = DecodeRtaskCommandRequest(encoded.Payload);
            VERIFY_IS_TRUE(decoded.Succeeded());
            VERIFY_ARE_EQUAL(request.CommandId, decoded.Value.CommandId);
            VERIFY_ARE_EQUAL(request.RunId, decoded.Value.RunId);
            VERIFY_ARE_EQUAL(request.Capability, decoded.Value.Capability);
            VERIFY_IS_TRUE(request.Method == decoded.Value.Method);
            VERIFY_ARE_EQUAL(request.Text, decoded.Value.Text);
            const auto hasText = request.Method == RtaskMethod::StatusBlocked ||
                                 request.Method == RtaskMethod::Result;
            VERIFY_ARE_EQUAL(
                hasText,
                encoded.Payload.find("\"text\"") != std::string::npos);
        }
    }

    void RtaskCommandJsonTests::CommandResultRoundTripsUnsignedCursors()
    {
        constexpr auto maximum = (std::numeric_limits<uint64_t>::max)();
        const RtaskCommandResult source{
            std::string{ CommandId },
            RtaskCommandDisposition::Replayed,
            maximum,
            maximum,
        };
        const auto encoded = EncodeRtaskCommandResult(source);
        VERIFY_IS_TRUE(encoded.Succeeded);
        const auto decoded = DecodeRtaskCommandResult(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(source.CommandId, decoded.Value.CommandId);
        VERIFY_IS_TRUE(decoded.Value.Disposition == RtaskCommandDisposition::Replayed);
        VERIFY_ARE_EQUAL(maximum, decoded.Value.TaskVersion);
        VERIFY_ARE_EQUAL(maximum, decoded.Value.EventSequence);
    }

    void RtaskCommandJsonTests::RepeatedEncodingPreservesIdempotencyIdentity()
    {
        const auto request = _request(RtaskMethod::Result, "stable result");
        const auto first = EncodeRtaskCommandRequest(request);
        const auto second = EncodeRtaskCommandRequest(request);
        VERIFY_IS_TRUE(first.Succeeded);
        VERIFY_IS_TRUE(second.Succeeded);
        VERIFY_ARE_EQUAL(first.Payload, second.Payload);
        const auto decoded = DecodeRtaskCommandRequest(first.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ CommandId }, decoded.Value.CommandId);
    }

    void RtaskCommandJsonTests::RejectsUnknownExtraDuplicateAndNumericDrift()
    {
        const auto source = _request(RtaskMethod::StatusInProgress);
        const auto encoded = EncodeRtaskCommandRequest(source);
        VERIFY_IS_TRUE(encoded.Succeeded);

        auto extra = encoded.Payload;
        extra.insert(extra.size() - 1, ",\"taskId\":\"forbidden\"");
        VERIFY_IS_TRUE(
            DecodeRtaskCommandRequest(extra).Status ==
            ProtocolRpcJsonStatus::InvalidSchema);
        auto unknown = encoded.Payload;
        const auto method = std::string{ "status_in_progress" };
        unknown.replace(unknown.find(method), method.size(), "status_done");
        VERIFY_IS_TRUE(
            DecodeRtaskCommandRequest(unknown).Status ==
            ProtocolRpcJsonStatus::InvalidSchema);

        constexpr std::string_view duplicate =
            R"JSON({"commandId":"a3000000-0000-4000-8000-000000000001","commandId":"a3000000-0000-4000-8000-000000000001","runId":"a4000000-0000-4000-8000-000000000001","capability":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","method":"status_in_progress"})JSON";
        VERIFY_IS_TRUE(
            DecodeRtaskCommandRequest(duplicate).Status ==
            ProtocolRpcJsonStatus::MalformedJson);

        constexpr std::string_view decimal =
            R"JSON({"commandId":"a3000000-0000-4000-8000-000000000001","disposition":"applied","taskVersion":1.0,"eventSequence":2})JSON";
        VERIFY_IS_TRUE(
            DecodeRtaskCommandResult(decimal).Status ==
            ProtocolRpcJsonStatus::InvalidSchema);
        constexpr std::string_view exponent =
            R"JSON({"commandId":"a3000000-0000-4000-8000-000000000001","disposition":"applied","taskVersion":1,"eventSequence":2e0})JSON";
        VERIFY_IS_TRUE(
            DecodeRtaskCommandResult(exponent).Status ==
            ProtocolRpcJsonStatus::InvalidSchema);
    }

    void RtaskCommandJsonTests::MalformedAndOversizedErrorsNeverEchoSecrets()
    {
        const auto capability = std::string(RtaskCapabilityBytes, 'c');
        const auto privateText = std::string{ "private-result-content" };
        const auto malformed =
            std::string{ "{\"capability\":\"" } + capability +
            "\",\"text\":\"" + privateText;
        const auto decoded = DecodeRtaskCommandRequest(malformed);
        VERIFY_IS_TRUE(decoded.Status == ProtocolRpcJsonStatus::MalformedJson);
        VERIFY_IS_TRUE(decoded.Error.find(capability) == std::string::npos);
        VERIFY_IS_TRUE(decoded.Error.find(privateText) == std::string::npos);

        const std::string oversized(MaximumControlFrameBytes + 1, 'x');
        const auto oversizedResult = DecodeRtaskCommandRequest(oversized);
        VERIFY_IS_TRUE(oversizedResult.Status == ProtocolRpcJsonStatus::Oversized);
        VERIFY_IS_LESS_THAN(oversizedResult.Error.size(), size_t{ 128 });

        std::string invalidUtf8{ "\xc0\x80", 2 };
        VERIFY_IS_TRUE(
            DecodeRtaskCommandRequest(invalidUtf8).Status ==
            ProtocolRpcJsonStatus::InvalidUtf8);
    }
}
