// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeProcessTestFixtures.h"

#include "RuntimeControlHandshake.h"

#include "ProtocolCapabilities.h"
#include "ProtocolRpcJson.h"

#include <atomic>
#include <limits>

using namespace WEX::TestExecution;

namespace TaskRuntimeLocalTests::ProcessFixtures
{
    ProductRuntimeTestIdentity::ProductRuntimeTestIdentity()
    {
        static std::atomic_uint64_t sequence{};
        const auto value = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (value > (std::numeric_limits<uint32_t>::max)())
        {
            throw std::runtime_error{ "Product Runtime test channel sequence exhausted" };
        }

        std::array<wchar_t, 33> channel{};
        swprintf_s(
            channel.data(),
            channel.size(),
            L"test-%08lx-%08llx",
            GetCurrentProcessId(),
            value);
        _channel = channel.data();
        _settingsRoot = _rootSeed.Path();
        _settingsRoot += L" settings root";
        _identity = TestFixtures::RequireIdentity(_settingsRoot, _channel);
    }

    ProductRuntimeTestIdentity::~ProductRuntimeTestIdentity()
    {
        std::error_code ignored;
        std::filesystem::remove_all(_settingsRoot, ignored);
    }

    const std::wstring& ProductRuntimeTestIdentity::Channel() const noexcept
    {
        return _channel;
    }

    const std::filesystem::path& ProductRuntimeTestIdentity::SettingsRoot() const noexcept
    {
        return _settingsRoot;
    }

    const RuntimeIdentity& ProductRuntimeTestIdentity::Identity() const noexcept
    {
        return _identity;
    }

    ProductControlClient RequireProductControlClient(
        const ProductRuntimeTestIdentity& identity,
        const std::string_view clientInstanceId)
    {
        ProductControlClient result;
        auto connected = ConnectRuntimeControlChannel(
            identity.Identity(),
            ProductControlTimeoutMilliseconds);
        VERIFY_IS_TRUE(connected.Succeeded());
        if (!connected.Succeeded())
        {
            return result;
        }

        HelloRequest request;
        request.ClientInstanceId = std::string{ clientInstanceId };
        request.ClientKind = ProtocolClientKind::TestPeer;
        request.ExpectedRuntimeIdentity = identity.Identity().ProtocolIdentity;
        request.OptionalCapabilities = static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
        auto handshake = PerformRuntimeClientHandshake(
            connected.Channel,
            request,
            ProductControlTimeoutMilliseconds);
        VERIFY_IS_TRUE(handshake.Succeeded());
        if (!handshake.Succeeded())
        {
            return result;
        }
        result.Channel = std::move(connected.Channel);
        result.Handshake = std::move(handshake.Response);
        return result;
    }

    ControlResponse RequireProductResponse(
        RuntimeControlChannel& channel,
        const std::string_view requestId,
        const ControlMethod method,
        std::string payloadJson)
    {
        ControlRequest request;
        request.RequestId = std::string{ requestId };
        request.Method = method;
        request.PayloadJson = std::move(payloadJson);
        const auto encoded = EncodeControlRequest(request);
        VERIFY_IS_TRUE(encoded.Succeeded);
        if (!encoded.Succeeded)
        {
            return {};
        }
        const auto sent = channel.Send(
            encoded.Payload,
            ProductControlTimeoutMilliseconds);
        VERIFY_IS_TRUE(sent.Succeeded());
        if (!sent.Succeeded())
        {
            return {};
        }
        const auto received = channel.Receive(ProductControlTimeoutMilliseconds);
        VERIFY_IS_TRUE(received.Succeeded());
        if (!received.Succeeded())
        {
            return {};
        }
        const auto decoded = DecodeControlResponse(received.Payload);
        VERIFY_IS_TRUE(decoded.Succeeded());
        return decoded.Value;
    }
}
