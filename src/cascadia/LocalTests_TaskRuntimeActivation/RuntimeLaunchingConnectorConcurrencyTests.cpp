// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeLaunchingConnectorTestSupport.h"

#include <algorithm>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeLaunchingConnectorConcurrencyTests
    {
        TEST_CLASS(RuntimeLaunchingConnectorConcurrencyTests);

        TEST_METHOD(ConcurrentClientsCoordinateOneLaunchAndAllConnect);
    };

    void RuntimeLaunchingConnectorConcurrencyTests::ConcurrentClientsCoordinateOneLaunchAndAllConnect()
    {
        constexpr size_t clientCount{ 12 };
        UniqueActivationIdentity identity;
        const auto direct = std::make_shared<FakeDirectConnector>();
        const auto locks = MakeRuntimeLaunchLockProvider();
        const auto launcher = std::make_shared<FakeProcessLauncher>();
        const auto services = MakeRuntimeActivationWindowsServices();
        VERIFY_IS_NOT_NULL(locks.get());
        VERIFY_IS_NOT_NULL(services.get());

        auto start = MakeTestEvent();
        auto allInitialConnects = MakeTestEvent();
        std::atomic_bool runtimeAvailable{};
        std::vector<DWORD> initialThreads;
        direct->SetHandler([&] {
            if (runtimeAvailable.load(std::memory_order_acquire))
            {
                return ConnectedResult();
            }

            const auto thread = GetCurrentThreadId();
            if (std::ranges::find(initialThreads, thread) == initialThreads.end())
            {
                initialThreads.emplace_back(thread);
                if (initialThreads.size() == clientCount)
                {
                    SetEvent(allInitialConnects.get());
                }
            }
            return AbsentResult();
        });

        const auto processApi = std::make_shared<FakeRuntimeWindowsProcessApi>();
        const auto concreteLauncher = MakeTestProcessLauncher(processApi);
        std::atomic_bool launchSawEveryClient{};
        launcher->Handler = [&](const RuntimeProcessLaunchRequest& request) {
            launchSawEveryClient.store(
                WaitForSingleObject(allInitialConnects.get(), 5000) == WAIT_OBJECT_0,
                std::memory_order_release);
            auto launched = concreteLauncher->Launch(request);
            if (launched.Started())
            {
                runtimeAvailable.store(true, std::memory_order_release);
            }
            return launched;
        };

        RuntimeLaunchingControlConnector connector{
            LaunchingConfiguration(),
            {
                direct,
                locks,
                launcher,
                services,
            },
        };
        std::vector<RuntimeControlConnectionResult> results(clientCount);
        std::vector<std::thread> clients;
        clients.reserve(clientCount);
        for (size_t index = 0; index < clientCount; ++index)
        {
            clients.emplace_back([&, index] {
                WaitForSingleObject(start.get(), 5000);
                results[index] = connector.Connect(
                    identity.Identity(),
                    LaunchingHello(identity.Identity()),
                    10000);
            });
        }
        SetEvent(start.get());
        for (auto& client : clients)
        {
            client.join();
        }

        VERIFY_IS_TRUE(launchSawEveryClient.load(std::memory_order_acquire));
        VERIFY_ARE_EQUAL(clientCount, initialThreads.size());
        VERIFY_ARE_EQUAL(size_t{ 1 }, launcher->Calls.load());
        for (size_t index = 0; index < results.size(); ++index)
        {
            const auto& result = results[index];
            if (!result.Succeeded())
            {
                const auto message =
                    L"Concurrent activation failure at client " +
                    std::to_wstring(index) + L": status=" +
                    std::to_wstring(static_cast<int>(result.Status)) + L", reason=" +
                    std::to_wstring(static_cast<int>(result.FailureReason)) + L", phase=" +
                    std::to_wstring(static_cast<int>(result.Phase)) + L", systemError=" +
                    std::to_wstring(result.SystemError) + L", error=" +
                    std::wstring{ result.Error.begin(), result.Error.end() };
                WEX::Logging::Log::Comment(message.c_str());
            }
            VERIFY_IS_TRUE(result.Succeeded());
        }
        VERIFY_ARE_EQUAL(
            size_t{ 1 },
            HandleCloseCount(*processApi, processApi->CreatedProcess.Process));
        VERIFY_IS_TRUE(processApi->TerminateHandles.empty());

        auto released = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(released.Acquired());
        if (released.Acquired())
        {
            VERIFY_IS_TRUE(
                released.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
        }
    }
}
