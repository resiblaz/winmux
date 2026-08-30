// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeFoundationRouter.h"
#include "TaskRuntimeTestFixtures.h"

#include "ProtocolCapabilities.h"
#include "RuntimeFoundationRpc.h"

#include <json/json.h>

#include <atomic>
#include <memory>
#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr std::string_view RuntimeInstanceId{ "74000000-0000-4000-8000-000000000001" };
        constexpr std::string_view ClientInstanceId{ "73000000-0000-4000-8000-000000000001" };
        constexpr uint32_t RuntimeProcessId{ 4242 };

        bool _parseObject(const std::string_view payload, Json::Value& root)
        {
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false;
            builder["allowTrailingCommas"] = false;
            builder["collectComments"] = false;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            builder["strictRoot"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            std::string error;
            return reader &&
                   reader->parse(payload.data(), payload.data() + payload.size(), &root, &error) &&
                   root.isObject();
        }

        Json::Value _requireObject(const std::string_view payload)
        {
            Json::Value root;
            VERIFY_IS_TRUE(_parseObject(payload, root));
            return root;
        }

        void _verifyExactMembers(
            const Json::Value& object,
            const std::initializer_list<std::string_view> members)
        {
            VERIFY_IS_TRUE(object.isObject());
            VERIFY_ARE_EQUAL(members.size(), static_cast<size_t>(object.size()));
            for (const auto member : members)
            {
                VERIFY_IS_TRUE(object.isMember(std::string{ member }));
            }
        }

        RuntimeControlClientContext _client(const std::string_view clientId = ClientInstanceId)
        {
            RuntimeControlClientContext client;
            client.ClientInstanceId = std::string{ clientId };
            client.ClientKind = ProtocolClientKind::TestPeer;
            client.NegotiatedCapabilities = static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
            return client;
        }

        RuntimeControlCancellationToken _cancellation(const bool requested = false)
        {
            return RuntimeControlCancellationToken{
                std::make_shared<std::atomic_bool>(requested)
            };
        }

        ControlRequest _request(const ControlMethod method, std::string payload = "{}")
        {
            ControlRequest request;
            request.Method = method;
            request.PayloadJson = std::move(payload);
            return request;
        }

        void _verifyTypedError(
            const RuntimeControlRouteResult& result,
            const ProtocolRpcErrorCode code)
        {
            VERIFY_IS_FALSE(result.Succeeded);
            VERIFY_IS_TRUE(result.Error.has_value());
            if (result.Error)
            {
                VERIFY_IS_TRUE(result.Error->Code == code);
                VERIFY_IS_FALSE(result.Error->Message.empty());
            }
        }

        RuntimeFoundationRouter _router(const RuntimeIdentity& identity)
        {
            return RuntimeFoundationRouter{
                identity.ProtocolIdentity,
                std::string{ RuntimeInstanceId },
                RuntimeProcessId
            };
        }
    }

    class RuntimeFoundationRouterTests
    {
        TEST_CLASS(RuntimeFoundationRouterTests);

        TEST_METHOD(PingAndRuntimeStatusReturnExactFoundationFields);
        TEST_METHOD(NonEmptyOrMalformedParametersReturnInvalidRequest);
        TEST_METHOD(CancellationReturnsRuntimeUnavailable);
        TEST_METHOD(UnsupportedMethodReturnsInvalidRequest);
        TEST_METHOD(ConcurrentReadOnlyRoutesRemainIndependent);
    };

    void RuntimeFoundationRouterTests::PingAndRuntimeStatusReturnExactFoundationFields()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto router = _router(identity);
        VERIFY_IS_TRUE(router.Valid());
        const auto client = _client();
        const auto cancellation = _cancellation();

        const auto ping = router.Route(
            client,
            _request(ControlMethod::Ping),
            cancellation);
        VERIFY_IS_TRUE(ping.Succeeded);
        VERIFY_IS_FALSE(ping.Error.has_value());
        const auto pingResult = DecodePingResult(ping.ResultJson);
        VERIFY_IS_TRUE(pingResult.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ RuntimeInstanceId }, pingResult.Value.RuntimeInstanceId);
        VERIFY_ARE_EQUAL(std::string{ ClientInstanceId }, pingResult.Value.ClientInstanceId);

        const auto status = router.Route(
            client,
            _request(ControlMethod::RuntimeStatus),
            cancellation);
        VERIFY_IS_TRUE(status.Succeeded);
        VERIFY_IS_FALSE(status.Error.has_value());
        const auto statusResult = DecodeRuntimeStatusResult(status.ResultJson);
        VERIFY_IS_TRUE(statusResult.Succeeded());
        VERIFY_ARE_EQUAL(identity.ProtocolIdentity, statusResult.Value.RuntimeIdentity);
        VERIFY_ARE_EQUAL(std::string{ RuntimeInstanceId }, statusResult.Value.RuntimeInstanceId);
        VERIFY_ARE_EQUAL(RuntimeProcessId, statusResult.Value.ProcessId);
        VERIFY_ARE_EQUAL(std::string{ RuntimeFoundationReadyState }, statusResult.Value.State);
    }

    void RuntimeFoundationRouterTests::NonEmptyOrMalformedParametersReturnInvalidRequest()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto router = _router(identity);
        const auto client = _client();
        const auto cancellation = _cancellation();
        const std::array cases{
            _request(ControlMethod::Ping, "{\"unexpected\":true}"),
            _request(ControlMethod::RuntimeStatus, "{"),
            _request(ControlMethod::Ping, "[]"),
        };

        for (const auto& request : cases)
        {
            const auto result = router.Route(client, request, cancellation);
            _verifyTypedError(result, ProtocolRpcErrorCode::InvalidRequest);
            if (result.Error)
            {
                const auto data = _requireObject(result.Error->DataJson);
                _verifyExactMembers(data, { "method", "requirement" });
                VERIFY_ARE_EQUAL(
                    std::string{ ControlMethodName(request.Method) },
                    data["method"].asString());
                VERIFY_ARE_EQUAL(
                    std::string{ "params_must_be_empty_object" },
                    data["requirement"].asString());
            }
        }
    }

    void RuntimeFoundationRouterTests::CancellationReturnsRuntimeUnavailable()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto router = _router(identity);
        const auto result = router.Route(
            _client(),
            _request(ControlMethod::Ping),
            _cancellation(true));
        _verifyTypedError(result, ProtocolRpcErrorCode::RuntimeUnavailable);
        if (result.Error)
        {
            const auto data = _requireObject(result.Error->DataJson);
            _verifyExactMembers(data, { "state" });
            VERIFY_ARE_EQUAL(std::string{ "stopping" }, data["state"].asString());
        }
    }

    void RuntimeFoundationRouterTests::UnsupportedMethodReturnsInvalidRequest()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto router = _router(identity);
        const auto result = router.Route(
            _client(),
            _request(ControlMethod::QueryTask),
            _cancellation());
        _verifyTypedError(result, ProtocolRpcErrorCode::InvalidRequest);
        if (result.Error)
        {
            const auto data = _requireObject(result.Error->DataJson);
            _verifyExactMembers(data, { "method" });
            VERIFY_ARE_EQUAL(std::string{ "query_task" }, data["method"].asString());
        }
    }

    void RuntimeFoundationRouterTests::ConcurrentReadOnlyRoutesRemainIndependent()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto router = _router(identity);
        constexpr size_t threadCount{ 12 };
        constexpr size_t routesPerThread{ 100 };
        wil::unique_handle allReady{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        wil::unique_handle start{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        VERIFY_IS_NOT_NULL(allReady.get());
        VERIFY_IS_NOT_NULL(start.get());
        if (!allReady || !start)
        {
            return;
        }

        std::atomic_size_t readyThreads{};
        std::atomic_size_t failures{};
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        try
        {
            for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
            {
                threads.emplace_back([&, threadIndex] {
                    if (readyThreads.fetch_add(1, std::memory_order_acq_rel) + 1 == threadCount)
                    {
                        (void)SetEvent(allReady.get());
                    }
                    if (WaitForSingleObject(start.get(), 5000) != WAIT_OBJECT_0)
                    {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }

                    std::array<char, 37> clientId{};
                    sprintf_s(
                        clientId.data(),
                        clientId.size(),
                        "73000000-0000-4000-8000-%012zu",
                        threadIndex + 1);
                    const auto client = _client(clientId.data());
                    const auto cancellation = _cancellation();
                    for (size_t routeIndex = 0; routeIndex < routesPerThread; ++routeIndex)
                    {
                        const auto method = routeIndex % 2 == 0 ?
                                                ControlMethod::Ping :
                                                ControlMethod::RuntimeStatus;
                        const auto result = router.Route(
                            client,
                            _request(method),
                            cancellation);
                        if (!result.Succeeded || result.Error)
                        {
                            failures.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        const auto valid = method == ControlMethod::Ping ?
                                               [&] {
                                                   const auto decoded = DecodePingResult(result.ResultJson);
                                                   return decoded.Succeeded() &&
                                                          decoded.Value.RuntimeInstanceId == RuntimeInstanceId &&
                                                          decoded.Value.ClientInstanceId == client.ClientInstanceId;
                                               }() :
                                               [&] {
                                                   const auto decoded = DecodeRuntimeStatusResult(result.ResultJson);
                                                   return decoded.Succeeded() &&
                                                          decoded.Value.RuntimeIdentity == identity.ProtocolIdentity &&
                                                          decoded.Value.RuntimeInstanceId == RuntimeInstanceId &&
                                                          decoded.Value.ProcessId == RuntimeProcessId &&
                                                          decoded.Value.State == RuntimeFoundationReadyState;
                                               }();
                        if (!valid)
                        {
                            failures.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                });
            }
        }
        catch (...)
        {
            (void)SetEvent(start.get());
            for (auto& thread : threads)
            {
                thread.join();
            }
            throw;
        }

        const auto ready = WaitForSingleObject(allReady.get(), 5000);
        (void)SetEvent(start.get());
        for (auto& thread : threads)
        {
            thread.join();
        }
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, ready);
        VERIFY_ARE_EQUAL(size_t{ 0 }, failures.load(std::memory_order_relaxed));
    }
}
