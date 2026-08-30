// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "SnapshotAndSubscribe.h"

#include <limits>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        constexpr std::string_view SubscriptionId{ "40000000-0000-4000-8000-000000000092" };

        std::string SnapshotResultJson(
            const std::string_view generation,
            const std::string_view cursor)
        {
            return "{\"subscriptionId\":\"" + std::string{ SubscriptionId } +
                   "\",\"generation\":" + std::string{ generation } +
                   ",\"cursor\":" + std::string{ cursor } +
                   ",\"snapshot\":{}}";
        }

        std::string UnsubscribeJson(const std::string_view generation)
        {
            return "{\"subscriptionId\":\"" + std::string{ SubscriptionId } +
                   "\",\"generation\":" + std::string{ generation } + "}";
        }
    }

    class SnapshotAndSubscribeTests
    {
        TEST_CLASS(SnapshotAndSubscribeTests);

        TEST_METHOD(SnapshotResultRoundTripsStrictIdentityCursorAndObject);
        TEST_METHOD(UnsubscribeRequestRoundTripsStrictIdentity);
        TEST_METHOD(PreservesMaximumUnsignedCursorValues);
        TEST_METHOD(RejectsNumericAndUtf8Boundaries);
        TEST_METHOD(RejectsMalformedUnknownAndOutOfBoundsValues);
    };

    void SnapshotAndSubscribeTests::SnapshotResultRoundTripsStrictIdentityCursorAndObject()
    {
        SnapshotAndSubscribeResult source;
        source.SubscriptionId = SubscriptionId;
        source.Generation = 7;
        source.Cursor = 99;
        source.SnapshotJson = "{\"tasks\":[{\"id\":1}],\"workspace\":{\"name\":\"测试\"}}";
        const auto encoded = EncodeSnapshotAndSubscribeResult(source);
        VERIFY_IS_TRUE(encoded.Succeeded);
        const auto decoded = DecodeSnapshotAndSubscribeResult(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(source.SubscriptionId, decoded.Value.SubscriptionId);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, decoded.Value.Generation);
        VERIFY_ARE_EQUAL(uint64_t{ 99 }, decoded.Value.Cursor);
        VERIFY_IS_TRUE(decoded.Value.SnapshotJson.find("\"tasks\"") != std::string::npos);
        VERIFY_IS_TRUE(decoded.Value.SnapshotJson.find("测试") != std::string::npos);
    }

    void SnapshotAndSubscribeTests::UnsubscribeRequestRoundTripsStrictIdentity()
    {
        UnsubscribeRequest source;
        source.SubscriptionId = SubscriptionId;
        source.Generation = 8;
        const auto encoded = EncodeUnsubscribeRequest(source);
        VERIFY_IS_TRUE(encoded.Succeeded);
        const auto decoded = DecodeUnsubscribeRequest(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(source.SubscriptionId, decoded.Value.SubscriptionId);
        VERIFY_ARE_EQUAL(uint64_t{ 8 }, decoded.Value.Generation);
    }

    void SnapshotAndSubscribeTests::PreservesMaximumUnsignedCursorValues()
    {
        constexpr auto maximum = std::numeric_limits<uint64_t>::max();
        SnapshotAndSubscribeResult source;
        source.SubscriptionId = SubscriptionId;
        source.Generation = maximum;
        source.Cursor = maximum;
        const auto encoded = EncodeSnapshotAndSubscribeResult(source);
        VERIFY_IS_TRUE(encoded.Succeeded);
        const auto decoded = DecodeSnapshotAndSubscribeResult(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(maximum, decoded.Value.Generation);
        VERIFY_ARE_EQUAL(maximum, decoded.Value.Cursor);

        const auto unsubscribe = EncodeUnsubscribeRequest({ std::string{ SubscriptionId }, maximum });
        VERIFY_IS_TRUE(unsubscribe.Succeeded);
        const auto decodedUnsubscribe = DecodeUnsubscribeRequest(unsubscribe.Payload);
        VERIFY_IS_TRUE(decodedUnsubscribe.Succeeded());
        VERIFY_ARE_EQUAL(maximum, decodedUnsubscribe.Value.Generation);
    }

    void SnapshotAndSubscribeTests::RejectsNumericAndUtf8Boundaries()
    {
        for (const auto invalid : {
                 std::string_view{ "-1" },
                 std::string_view{ "1.5" },
                 std::string_view{ "1.0" },
                 std::string_view{ "1e0" },
                 std::string_view{ "18446744073709551616" },
             })
        {
            VERIFY_IS_FALSE(DecodeSnapshotAndSubscribeResult(
                                SnapshotResultJson(invalid, "0"))
                                .Succeeded());
            VERIFY_IS_FALSE(DecodeSnapshotAndSubscribeResult(
                                SnapshotResultJson("1", invalid))
                                .Succeeded());
            VERIFY_IS_FALSE(DecodeUnsubscribeRequest(UnsubscribeJson(invalid)).Succeeded());
        }

        std::string invalidUtf8{ "{" };
        invalidUtf8.push_back(static_cast<char>(0xff));
        invalidUtf8.push_back('}');
        VERIFY_IS_TRUE(DecodeSnapshotAndSubscribeResult(invalidUtf8).Status ==
                       ProtocolRpcJsonStatus::InvalidUtf8);
        VERIFY_IS_TRUE(DecodeUnsubscribeRequest(invalidUtf8).Status ==
                       ProtocolRpcJsonStatus::InvalidUtf8);

        SnapshotAndSubscribeResult invalidSnapshot;
        invalidSnapshot.SubscriptionId = SubscriptionId;
        invalidSnapshot.Generation = 1;
        invalidSnapshot.SnapshotJson = "{\"value\":\"";
        invalidSnapshot.SnapshotJson.push_back(static_cast<char>(0xff));
        invalidSnapshot.SnapshotJson += "\"}";
        VERIFY_IS_FALSE(EncodeSnapshotAndSubscribeResult(invalidSnapshot).Succeeded);
    }

    void SnapshotAndSubscribeTests::RejectsMalformedUnknownAndOutOfBoundsValues()
    {
        VERIFY_IS_TRUE(DecodeSnapshotAndSubscribeResult(
                           "{\"subscriptionId\":\"40000000-0000-4000-8000-000000000092\","
                           "\"generation\":1,\"cursor\":0,\"snapshot\":{},\"extra\":1}")
                           .Status == ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeSnapshotAndSubscribeResult(
                           "{\"subscriptionId\":\"bad\",\"generation\":1,\"cursor\":0,\"snapshot\":{}}")
                           .Status == ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeSnapshotAndSubscribeResult(
                           "{\"subscriptionId\":\"40000000-0000-4000-8000-000000000092\","
                           "\"generation\":0,\"cursor\":0,\"snapshot\":{}}")
                           .Status == ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeUnsubscribeRequest(
                           "{\"subscriptionId\":\"40000000-0000-4000-8000-000000000092\","
                           "\"generation\":1,\"extra\":true}")
                           .Status == ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeUnsubscribeRequest("{\"subscriptionId\":").Status ==
                       ProtocolRpcJsonStatus::MalformedJson);

        SnapshotAndSubscribeResult invalid;
        invalid.SubscriptionId = SubscriptionId;
        invalid.Generation = 1;
        invalid.SnapshotJson = "[]";
        VERIFY_IS_FALSE(EncodeSnapshotAndSubscribeResult(invalid).Succeeded);
        VERIFY_IS_FALSE(EncodeUnsubscribeRequest({ std::string{ SubscriptionId }, 0 }).Succeeded);
    }
}
