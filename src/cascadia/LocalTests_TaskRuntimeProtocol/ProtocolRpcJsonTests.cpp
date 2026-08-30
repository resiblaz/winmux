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
        constexpr std::string_view RequestId{ "30000000-0000-4000-8000-000000000003" };
        constexpr std::string_view SubscriptionId{ "40000000-0000-4000-8000-000000000004" };
    }

    class ProtocolRpcJsonTests
    {
        TEST_CLASS(ProtocolRpcJsonTests);

        TEST_METHOD(MethodAndErrorNamesMapToCapabilities);
        TEST_METHOD(RequestRoundTripsStrictObjectPayload);
        TEST_METHOD(SuccessAndErrorResponsesRoundTripExclusively);
        TEST_METHOD(NotificationRoundTripsAndUnknownKindIsIgnorable);
        TEST_METHOD(RejectsInvalidIdsCursorsBodiesAndExtraFields);
    };

    void ProtocolRpcJsonTests::MethodAndErrorNamesMapToCapabilities()
    {
        VERIFY_IS_TRUE(ControlMethodFromName("query_task") == std::optional{ ControlMethod::QueryTask });
        VERIFY_IS_FALSE(ControlMethodFromName("future_method").has_value());
        VERIFY_IS_TRUE(
            RequiredCapabilities(ControlMethod::QueryTask) ==
            static_cast<uint64_t>(ProtocolCapability::TaskQueries));
        VERIFY_IS_TRUE(
            RequiredCapabilities(ControlMethod::AcquireInputLease) ==
            static_cast<uint64_t>(ProtocolCapability::InputControllerLease));
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, RequiredCapabilities(ControlMethod::Ping));
        VERIFY_IS_TRUE(
            ProtocolRpcErrorCodeFromName("duplicate_request") ==
            std::optional{ ProtocolRpcErrorCode::DuplicateRequest });
    }

    void ProtocolRpcJsonTests::RequestRoundTripsStrictObjectPayload()
    {
        ControlRequest request;
        request.RequestId = RequestId;
        request.Method = ControlMethod::SnapshotAndSubscribe;
        request.PayloadJson = R"({"workspace":"current","after":12})";
        const auto encoded = EncodeControlRequest(request);
        VERIFY_IS_TRUE(encoded.Succeeded);

        const auto decoded = DecodeControlRequest(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ RequestId }, decoded.Value.RequestId);
        VERIFY_IS_TRUE(decoded.Value.Method == ControlMethod::SnapshotAndSubscribe);
        VERIFY_IS_TRUE(decoded.Value.PayloadJson.find("\"after\":12") != std::string::npos);

        request.PayloadJson = "[]";
        VERIFY_IS_FALSE(EncodeControlRequest(request).Succeeded);
        request.PayloadJson = "{\"duplicate\":1,\"duplicate\":2}";
        VERIFY_IS_FALSE(EncodeControlRequest(request).Succeeded);
    }

    void ProtocolRpcJsonTests::SuccessAndErrorResponsesRoundTripExclusively()
    {
        ControlResponse success;
        success.RequestId = RequestId;
        success.Succeeded = true;
        success.ResultJson = R"({"pong":true})";
        auto encoded = EncodeControlResponse(success);
        VERIFY_IS_TRUE(encoded.Succeeded);
        auto decoded = DecodeControlResponse(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_IS_TRUE(decoded.Value.Succeeded);
        VERIFY_IS_FALSE(decoded.Value.Error.has_value());

        ControlResponse failure;
        failure.RequestId = RequestId;
        failure.Error = ProtocolRpcError{
            ProtocolRpcErrorCode::Conflict,
            "Task version conflict",
            R"({"expected":2,"actual":3})",
        };
        encoded = EncodeControlResponse(failure);
        VERIFY_IS_TRUE(encoded.Succeeded);
        decoded = DecodeControlResponse(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_IS_FALSE(decoded.Value.Succeeded);
        VERIFY_IS_TRUE(decoded.Value.Error.has_value());
        VERIFY_IS_TRUE(decoded.Value.Error->Code == ProtocolRpcErrorCode::Conflict);
        VERIFY_ARE_EQUAL(std::string{ "Task version conflict" }, decoded.Value.Error->Message);

        failure.Succeeded = true;
        VERIFY_IS_FALSE(EncodeControlResponse(failure).Succeeded);
    }

    void ProtocolRpcJsonTests::NotificationRoundTripsAndUnknownKindIsIgnorable()
    {
        ControlNotification notification;
        notification.SubscriptionId = SubscriptionId;
        notification.Generation = 2;
        notification.Sequence = 9;
        notification.Kind = ControlNotificationKind::ProjectionChanged;
        notification.PayloadJson = R"({"taskVersion":7})";
        const auto encoded = EncodeControlNotification(notification);
        VERIFY_IS_TRUE(encoded.Succeeded);
        auto decoded = DecodeControlNotification(encoded.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(uint64_t{ 9 }, decoded.Value.Sequence);

        auto future = encoded.Payload;
        const auto event = std::string{ "projection_changed" };
        future.replace(future.find(event), event.size(), "future_optional_event");
        decoded = DecodeControlNotification(future);
        VERIFY_IS_TRUE(decoded.Status == ProtocolRpcJsonStatus::UnknownNotification);
        VERIFY_ARE_EQUAL(std::string{ "future_optional_event" }, decoded.Value.UnknownKind);
        VERIFY_ARE_EQUAL(uint64_t{ 9 }, decoded.Value.Sequence);
    }

    void ProtocolRpcJsonTests::RejectsInvalidIdsCursorsBodiesAndExtraFields()
    {
        constexpr std::string_view unknownMethod = R"JSON({"type":"request","requestId":"30000000-0000-4000-8000-000000000003","method":"future","params":{}})JSON";
        VERIFY_IS_TRUE(DecodeControlRequest(unknownMethod).Status == ProtocolRpcJsonStatus::InvalidSchema);

        constexpr std::string_view invalidNotification = R"JSON({"type":"notification","subscriptionId":"40000000-0000-4000-8000-000000000004","generation":0,"sequence":1,"event":"projection_changed","payload":{}})JSON";
        VERIFY_IS_TRUE(DecodeControlNotification(invalidNotification).Status == ProtocolRpcJsonStatus::InvalidSchema);

        ControlNotification notification;
        notification.SubscriptionId = SubscriptionId;
        notification.Generation = 1;
        notification.Sequence = 1;
        auto encoded = EncodeControlNotification(notification);
        auto extra = encoded.Payload;
        extra.insert(extra.size() - 1, ",\"extra\":true");
        VERIFY_IS_TRUE(DecodeControlNotification(extra).Status == ProtocolRpcJsonStatus::InvalidSchema);

        ControlResponse failure;
        failure.RequestId = RequestId;
        failure.Error = ProtocolRpcError{
            ProtocolRpcErrorCode::InternalError,
            std::string(MaximumRpcErrorMessageBytes + 1, 'x'),
            "{}",
        };
        VERIFY_IS_FALSE(EncodeControlResponse(failure).Succeeded);
    }
}
