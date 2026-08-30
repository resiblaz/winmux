// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeFoundationRpc.h"

#include "ProtocolVersion.h"

#include <limits>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        constexpr std::string_view RuntimeIdentity{ "wt-task-runtime-v1:0123456789abcdef" };
        constexpr std::string_view RuntimeInstanceId{ "20000000-0000-4000-8000-000000000020" };
        constexpr std::string_view ClientInstanceId{ "10000000-0000-4000-8000-000000000011" };

        PingResult _ping()
        {
            return {
                std::string{ RuntimeInstanceId },
                std::string{ ClientInstanceId },
            };
        }

        RuntimeStatusResult _runtimeStatus(const uint32_t processId = 4242)
        {
            return {
                std::string{ RuntimeIdentity },
                std::string{ RuntimeInstanceId },
                processId,
                std::string{ RuntimeFoundationReadyState },
            };
        }

        std::string _pingJson(
            const std::string_view runtimeInstanceId,
            const std::string_view clientInstanceId)
        {
            return "{\"runtimeInstanceId\":\"" + std::string{ runtimeInstanceId } +
                   "\",\"clientInstanceId\":\"" + std::string{ clientInstanceId } + "\"}";
        }

        std::string _runtimeStatusJson(
            const std::string_view runtimeIdentity,
            const std::string_view runtimeInstanceId,
            const std::string_view processId,
            const std::string_view stateJson = "\"ready\"")
        {
            return "{\"runtimeIdentity\":\"" + std::string{ runtimeIdentity } +
                   "\",\"runtimeInstanceId\":\"" + std::string{ runtimeInstanceId } +
                   "\",\"processId\":" + std::string{ processId } +
                   ",\"state\":" + std::string{ stateJson } + "}";
        }
    }

    class RuntimeFoundationRpcTests
    {
        TEST_CLASS(RuntimeFoundationRpcTests);

        TEST_METHOD(PingResultRoundTripsCanonicalExactSchema);
        TEST_METHOD(RuntimeStatusRoundTripsCanonicalExactSchemaAndProcessBounds);
        TEST_METHOD(PingRejectsMissingUnknownWrongTypeAndInvalidIdentity);
        TEST_METHOD(RuntimeStatusRejectsMissingUnknownWrongTypeAndInvalidValues);
        TEST_METHOD(DecodersRejectMalformedUtf8AndOversizedPayloads);
        TEST_METHOD(EncodersRejectEveryInvalidField);
    };

    void RuntimeFoundationRpcTests::PingResultRoundTripsCanonicalExactSchema()
    {
        const auto source = _ping();
        const auto encoded = EncodePingResult(source);
        VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_IS_TRUE(encoded.Payload.find("\"runtimeInstanceId\"") != std::string::npos);
        VERIFY_IS_TRUE(encoded.Payload.find("\"clientInstanceId\"") != std::string::npos);

        const auto decoded = DecodePingResult(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(source.RuntimeInstanceId, decoded.Value.RuntimeInstanceId);
        VERIFY_ARE_EQUAL(source.ClientInstanceId, decoded.Value.ClientInstanceId);
    }

    void RuntimeFoundationRpcTests::RuntimeStatusRoundTripsCanonicalExactSchemaAndProcessBounds()
    {
        const auto source = _runtimeStatus();
        const auto encoded = EncodeRuntimeStatusResult(source);
        VERIFY_IS_TRUE(encoded.Succeeded);
        const auto decoded = DecodeRuntimeStatusResult(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(source.RuntimeIdentity, decoded.Value.RuntimeIdentity);
        VERIFY_ARE_EQUAL(source.RuntimeInstanceId, decoded.Value.RuntimeInstanceId);
        VERIFY_ARE_EQUAL(uint32_t{ 4242 }, decoded.Value.ProcessId);
        VERIFY_ARE_EQUAL(std::string{ RuntimeFoundationReadyState }, decoded.Value.State);

        constexpr auto maximumProcessId = std::numeric_limits<uint32_t>::max();
        const auto maximumEncoded = EncodeRuntimeStatusResult(_runtimeStatus(maximumProcessId));
        VERIFY_IS_TRUE(maximumEncoded.Succeeded);
        const auto maximumDecoded = DecodeRuntimeStatusResult(maximumEncoded.Payload);
        VERIFY_IS_TRUE(maximumDecoded.Succeeded());
        VERIFY_ARE_EQUAL(maximumProcessId, maximumDecoded.Value.ProcessId);

        auto maximumIdentity = _runtimeStatus();
        maximumIdentity.RuntimeIdentity.assign(128, 'x');
        const auto maximumIdentityEncoded = EncodeRuntimeStatusResult(maximumIdentity);
        VERIFY_IS_TRUE(maximumIdentityEncoded.Succeeded);
        VERIFY_IS_TRUE(DecodeRuntimeStatusResult(maximumIdentityEncoded.Payload).Succeeded());
    }

    void RuntimeFoundationRpcTests::PingRejectsMissingUnknownWrongTypeAndInvalidIdentity()
    {
        const std::vector<std::string> invalidSchemas{
            "[]",
            "{}",
            "{\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\"}",
            "{\"clientInstanceId\":\"10000000-0000-4000-8000-000000000011\"}",
            "{\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\","
            "\"clientInstanceId\":\"10000000-0000-4000-8000-000000000011\",\"extra\":true}",
            "{\"runtimeInstanceId\":1,"
            "\"clientInstanceId\":\"10000000-0000-4000-8000-000000000011\"}",
            "{\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\","
            "\"clientInstanceId\":null}",
        };
        for (const auto& payload : invalidSchemas)
        {
            VERIFY_IS_TRUE(DecodePingResult(payload).Status == ProtocolRpcJsonStatus::InvalidSchema);
        }

        for (const auto invalidId : {
                 std::string_view{ "00000000-0000-0000-0000-000000000000" },
                 std::string_view{ "20000000-0000-4000-8000-00000000002A" },
                 std::string_view{ "not-a-canonical-uuid" },
             })
        {
            VERIFY_IS_TRUE(DecodePingResult(_pingJson(invalidId, ClientInstanceId)).Status ==
                           ProtocolRpcJsonStatus::InvalidSchema);
            VERIFY_IS_TRUE(DecodePingResult(_pingJson(RuntimeInstanceId, invalidId)).Status ==
                           ProtocolRpcJsonStatus::InvalidSchema);
        }
    }

    void RuntimeFoundationRpcTests::RuntimeStatusRejectsMissingUnknownWrongTypeAndInvalidValues()
    {
        const std::vector<std::string> invalidSchemas{
            "[]",
            "{}",
            "{\"runtimeIdentity\":\"wt-task-runtime-v1:0123456789abcdef\","
            "\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\",\"processId\":1}",
            "{\"runtimeIdentity\":\"wt-task-runtime-v1:0123456789abcdef\","
            "\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\","
            "\"processId\":1,\"state\":\"ready\",\"extra\":true}",
            "{\"runtimeIdentity\":1,"
            "\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\","
            "\"processId\":1,\"state\":\"ready\"}",
            "{\"runtimeIdentity\":\"wt-task-runtime-v1:0123456789abcdef\","
            "\"runtimeInstanceId\":null,\"processId\":1,\"state\":\"ready\"}",
            "{\"runtimeIdentity\":\"wt-task-runtime-v1:0123456789abcdef\","
            "\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\","
            "\"processId\":1,\"state\":false}",
        };
        for (const auto& payload : invalidSchemas)
        {
            VERIFY_IS_TRUE(DecodeRuntimeStatusResult(payload).Status ==
                           ProtocolRpcJsonStatus::InvalidSchema);
        }

        for (const auto invalidProcessId : {
                 std::string_view{ "0" },
                 std::string_view{ "-1" },
                 std::string_view{ "1.0" },
                 std::string_view{ "1.5" },
                 std::string_view{ "1e0" },
                 std::string_view{ "4294967296" },
                 std::string_view{ "18446744073709551616" },
                 std::string_view{ "\"1\"" },
                 std::string_view{ "null" },
             })
        {
            VERIFY_IS_FALSE(DecodeRuntimeStatusResult(
                                _runtimeStatusJson(
                                    RuntimeIdentity,
                                    RuntimeInstanceId,
                                    invalidProcessId))
                                .Succeeded());
        }

        VERIFY_IS_TRUE(DecodeRuntimeStatusResult(
                           _runtimeStatusJson("", RuntimeInstanceId, "1"))
                           .Status == ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeRuntimeStatusResult(
                           _runtimeStatusJson("invalid identity!", RuntimeInstanceId, "1"))
                           .Status == ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeRuntimeStatusResult(
                           _runtimeStatusJson(std::string(129, 'x'), RuntimeInstanceId, "1"))
                           .Status == ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeRuntimeStatusResult(
                           _runtimeStatusJson(
                               RuntimeIdentity,
                               "00000000-0000-0000-0000-000000000000",
                               "1"))
                           .Status == ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeRuntimeStatusResult(
                           _runtimeStatusJson(RuntimeIdentity, RuntimeInstanceId, "1", "\"stopping\""))
                           .Status == ProtocolRpcJsonStatus::InvalidSchema);
    }

    void RuntimeFoundationRpcTests::DecodersRejectMalformedUtf8AndOversizedPayloads()
    {
        for (const auto malformed : {
                 std::string_view{ "{" },
                 std::string_view{
                     "{\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\","
                     "\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\","
                     "\"clientInstanceId\":\"10000000-0000-4000-8000-000000000011\"}" },
                 std::string_view{
                     "{\"runtimeIdentity\":\"wt-task-runtime-v1:0123456789abcdef\","
                     "\"runtimeInstanceId\":\"20000000-0000-4000-8000-000000000020\","
                     "\"processId\":1,\"state\":\"ready\"} trailing" },
             })
        {
            VERIFY_IS_TRUE(DecodePingResult(malformed).Status == ProtocolRpcJsonStatus::MalformedJson);
            VERIFY_IS_TRUE(DecodeRuntimeStatusResult(malformed).Status ==
                           ProtocolRpcJsonStatus::MalformedJson);
        }

        std::string invalidUtf8{ "{" };
        invalidUtf8.push_back(static_cast<char>(0xff));
        invalidUtf8.push_back('}');
        VERIFY_IS_TRUE(DecodePingResult(invalidUtf8).Status == ProtocolRpcJsonStatus::InvalidUtf8);
        VERIFY_IS_TRUE(DecodeRuntimeStatusResult(invalidUtf8).Status ==
                       ProtocolRpcJsonStatus::InvalidUtf8);

        const std::string oversized(MaximumControlFrameBytes + 1, 'x');
        VERIFY_IS_TRUE(DecodePingResult(oversized).Status == ProtocolRpcJsonStatus::Oversized);
        VERIFY_IS_TRUE(DecodeRuntimeStatusResult(oversized).Status ==
                       ProtocolRpcJsonStatus::Oversized);
    }

    void RuntimeFoundationRpcTests::EncodersRejectEveryInvalidField()
    {
        auto ping = _ping();
        ping.RuntimeInstanceId = "bad";
        VERIFY_IS_FALSE(EncodePingResult(ping).Succeeded);
        ping = _ping();
        ping.ClientInstanceId = "00000000-0000-0000-0000-000000000000";
        VERIFY_IS_FALSE(EncodePingResult(ping).Succeeded);

        auto status = _runtimeStatus();
        status.RuntimeIdentity.clear();
        VERIFY_IS_FALSE(EncodeRuntimeStatusResult(status).Succeeded);
        status = _runtimeStatus();
        status.RuntimeIdentity.assign(129, 'x');
        VERIFY_IS_FALSE(EncodeRuntimeStatusResult(status).Succeeded);
        status = _runtimeStatus();
        status.RuntimeInstanceId = "bad";
        VERIFY_IS_FALSE(EncodeRuntimeStatusResult(status).Succeeded);
        status = _runtimeStatus();
        status.ProcessId = 0;
        VERIFY_IS_FALSE(EncodeRuntimeStatusResult(status).Succeeded);
        status = _runtimeStatus();
        status.State = "stopping";
        VERIFY_IS_FALSE(EncodeRuntimeStatusResult(status).Succeeded);
    }
}
