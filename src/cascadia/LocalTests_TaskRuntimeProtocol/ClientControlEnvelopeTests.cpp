// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ProtocolRpcJson.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        constexpr std::string_view RequestId{ "30000000-0000-4000-8000-000000000091" };
        constexpr std::string_view SubscriptionId{ "40000000-0000-4000-8000-000000000091" };
    }

    class ClientControlEnvelopeTests
    {
        TEST_CLASS(ClientControlEnvelopeTests);

        TEST_METHOD(DecodesResponseIntoVariant);
        TEST_METHOD(DecodesKnownAndUnknownNotificationsIntoVariant);
        TEST_METHOD(RejectsRequestsUnknownTypesAndMalformedPayloads);
    };

    void ClientControlEnvelopeTests::DecodesResponseIntoVariant()
    {
        ControlResponse response;
        response.RequestId = RequestId;
        response.Succeeded = true;
        response.ResultJson = "{\"value\":1}";
        const auto encoded = EncodeControlResponse(response);
        VERIFY_IS_TRUE(encoded.Succeeded);
        const auto decoded = DecodeClientControlEnvelope(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_IS_TRUE(std::holds_alternative<ControlResponse>(decoded.Value));
        const auto& value = std::get<ControlResponse>(decoded.Value);
        VERIFY_ARE_EQUAL(std::string{ RequestId }, value.RequestId);
        VERIFY_IS_TRUE(value.Succeeded);
    }

    void ClientControlEnvelopeTests::DecodesKnownAndUnknownNotificationsIntoVariant()
    {
        ControlNotification notification;
        notification.SubscriptionId = SubscriptionId;
        notification.Generation = 2;
        notification.Sequence = 3;
        notification.PayloadJson = "{\"delta\":true}";
        const auto encoded = EncodeControlNotification(notification);
        VERIFY_IS_TRUE(encoded.Succeeded);
        auto decoded = DecodeClientControlEnvelope(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_IS_TRUE(std::holds_alternative<ControlNotification>(decoded.Value));
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, std::get<ControlNotification>(decoded.Value).Sequence);

        const std::string unknown =
            "{\"type\":\"notification\",\"subscriptionId\":\"" +
            std::string{ SubscriptionId } +
            "\",\"generation\":2,\"sequence\":4,\"event\":\"future_event\",\"payload\":{}}";
        decoded = DecodeClientControlEnvelope(unknown);
        VERIFY_IS_TRUE(decoded.Status == ProtocolRpcJsonStatus::UnknownNotification);
        VERIFY_IS_TRUE(std::holds_alternative<ControlNotification>(decoded.Value));
        const auto& future = std::get<ControlNotification>(decoded.Value);
        VERIFY_ARE_EQUAL(std::string{ "future_event" }, future.UnknownKind);
        VERIFY_ARE_EQUAL(uint64_t{ 4 }, future.Sequence);
    }

    void ClientControlEnvelopeTests::RejectsRequestsUnknownTypesAndMalformedPayloads()
    {
        ControlRequest request;
        request.RequestId = RequestId;
        const auto encoded = EncodeControlRequest(request);
        VERIFY_IS_TRUE(encoded.Succeeded);
        VERIFY_IS_TRUE(DecodeClientControlEnvelope(encoded.Payload).Status ==
                       ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeClientControlEnvelope("{\"type\":\"future\"}").Status ==
                       ProtocolRpcJsonStatus::InvalidSchema);
        VERIFY_IS_TRUE(DecodeClientControlEnvelope("{\"type\":").Status ==
                       ProtocolRpcJsonStatus::MalformedJson);
        VERIFY_IS_TRUE(DecodeClientControlEnvelope(std::string(1024 * 1024 + 1, 'x')).Status ==
                       ProtocolRpcJsonStatus::Oversized);
    }
}
