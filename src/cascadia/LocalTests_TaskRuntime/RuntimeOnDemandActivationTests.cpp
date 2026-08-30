// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeOnDemandActivationTestSupport.h"

#include "RuntimeActivationServices.h"
#include "RuntimeLaunchLockProvider.h"
#include "RuntimeLaunchingConnector.h"
#include "RuntimeProcessLauncher.h"

#include "ProtocolCapabilities.h"

#include <atomic>
#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::ProcessFixtures;
using namespace TaskRuntimeLocalTests::OnDemandActivation;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        class CountingProcessLauncher final : public IRuntimeProcessLauncher
        {
        public:
            explicit CountingProcessLauncher(
                std::shared_ptr<IRuntimeProcessLauncher> inner) noexcept :
                _inner{ std::move(inner) }
            {
            }

            RuntimeProcessLaunchResult Launch(
                const RuntimeProcessLaunchRequest& request) noexcept override
            {
                Calls.fetch_add(1, std::memory_order_relaxed);
                return _inner->Launch(request);
            }

            std::atomic_size_t Calls{};

        private:
            std::shared_ptr<IRuntimeProcessLauncher> _inner;
        };

        HelloRequest _activationHello(
            const ProductRuntimeTestIdentity& identity,
            const size_t index)
        {
            HelloRequest request;
            request.ClientInstanceId = ActivationClientInstanceId(index);
            request.ClientKind = ProtocolClientKind::TestPeer;
            request.ExpectedRuntimeIdentity = identity.Identity().ProtocolIdentity;
            request.OptionalCapabilities =
                static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
            return request;
        }

        bool _waitParent(
            ActivationParentProcess& parent,
            const DWORD expectedExitCode = 0)
        {
            DWORD exitCode{};
            const auto exited = parent.WaitForExit(
                ActivationParentExitTimeoutMilliseconds,
                exitCode);
            VERIFY_IS_TRUE(exited);
            if (exited)
            {
                VERIFY_ARE_EQUAL(expectedExitCode, exitCode);
            }
            return exited && exitCode == expectedExitCode;
        }

        void _verifyRuntimeStatus(ActiveRuntimeProbe& probe)
        {
            const auto response = RequireProductResponse(
                probe.Client.Channel,
                "92000000-0000-4000-8000-000000000001",
                ControlMethod::RuntimeStatus);
            VERIFY_IS_TRUE(response.Succeeded);
        }
    }

    class RuntimeOnDemandActivationTests
    {
        TEST_CLASS(RuntimeOnDemandActivationTests);

        TEST_METHOD(ProductClientActivationOutlivesShortLivedParent);
        TEST_METHOD(ConcurrentThreadsLaunchExactlyOneRealRuntime);
        TEST_METHOD(ConcurrentProcessesLaunchExactlyOneRealRuntime);
        TEST_METHOD(CrashedRuntimeRestartsWithNewProcessIdentity);
    };

    void RuntimeOnDemandActivationTests::ProductClientActivationOutlivesShortLivedParent()
    {
        ProductRuntimeTestIdentity identity;
        auto launched = LaunchActivationParent(
            identity,
            { ActivationClientInstanceId(0) });
        VERIFY_IS_TRUE(launched.Succeeded());
        if (!launched.Succeeded() || !_waitParent(launched.Process))
        {
            return;
        }

        auto probe = RequireActiveRuntime(
            identity,
            ActivationClientInstanceId(100));
        ActiveRuntimeProcessGuard runtime{ probe.ProcessId };
        VERIFY_IS_TRUE(static_cast<bool>(runtime));
        VERIFY_ARE_NOT_EQUAL(launched.Process.ProcessId(), runtime.ProcessId());
        Sleep(100);
        VERIFY_IS_TRUE(runtime.Running());
        _verifyRuntimeStatus(probe);
        probe.Client.Channel.Close();
        VERIFY_IS_TRUE(runtime.TerminateAndWait());
    }

    void RuntimeOnDemandActivationTests::ConcurrentThreadsLaunchExactlyOneRealRuntime()
    {
        constexpr size_t clientCount{ 12 };
        ProductRuntimeTestIdentity identity;
        const auto runtimeExecutable =
            ProcessFixtures::details::TaskRuntimeExecutable();
        const auto innerLauncher = MakeRuntimeProcessLauncher();
        VERIFY_IS_NOT_NULL(innerLauncher.get());
        if (!innerLauncher)
        {
            return;
        }
        const auto launcher = std::make_shared<CountingProcessLauncher>(
            innerLauncher);
        const auto direct = std::make_shared<RuntimeNamedPipeControlConnector>(
            RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent);
        const auto locks = MakeRuntimeLaunchLockProvider();
        const auto services = MakeRuntimeActivationWindowsServices();
        VERIFY_IS_NOT_NULL(launcher.get());
        VERIFY_IS_NOT_NULL(locks.get());
        VERIFY_IS_NOT_NULL(services.get());

        RuntimeLaunchingControlConnector connector{
            {
                runtimeExecutable,
                runtimeExecutable.parent_path(),
            },
            {
                direct,
                locks,
                launcher,
                services,
            },
        };
        ProcessFixtures::details::HandleOwner start{
            CreateEventW(nullptr, TRUE, FALSE, nullptr)
        };
        VERIFY_IS_TRUE(static_cast<bool>(start));
        if (!start)
        {
            return;
        }
        std::vector<RuntimeControlConnectionResult> results(clientCount);
        std::vector<std::thread> clients;
        clients.reserve(clientCount);
        for (size_t index = 0; index < clientCount; ++index)
        {
            clients.emplace_back([&, index] {
                (void)WaitForSingleObject(start.Get(), 5000);
                results[index] = connector.Connect(
                    identity.Identity(),
                    _activationHello(identity, index),
                    15000);
            });
        }
        SetEvent(start.Get());
        for (auto& client : clients)
        {
            client.join();
        }

        VERIFY_ARE_EQUAL(size_t{ 1 }, launcher->Calls.load());
        for (auto& result : results)
        {
            VERIFY_IS_TRUE(result.Succeeded());
            if (result.Connection)
            {
                result.Connection->Close();
            }
        }
        auto probe = RequireActiveRuntime(
            identity,
            ActivationClientInstanceId(101));
        ActiveRuntimeProcessGuard runtime{ probe.ProcessId };
        VERIFY_IS_TRUE(static_cast<bool>(runtime));
        probe.Client.Channel.Close();
        VERIFY_IS_TRUE(runtime.TerminateAndWait());
    }

    void RuntimeOnDemandActivationTests::ConcurrentProcessesLaunchExactlyOneRealRuntime()
    {
        constexpr size_t clientCount{ 8 };
        ProductRuntimeTestIdentity identity;
        const auto startName = ActivationStartEventName(identity);
        ProcessFixtures::details::HandleOwner start{
            CreateEventW(nullptr, TRUE, FALSE, startName.c_str())
        };
        VERIFY_IS_TRUE(static_cast<bool>(start));
        if (!start)
        {
            return;
        }

        std::vector<std::unique_ptr<TemporaryActivationRecord>> records;
        std::vector<ActivationParentProcess> parents;
        records.reserve(clientCount);
        parents.reserve(clientCount);
        for (size_t index = 0; index < clientCount; ++index)
        {
            auto record = std::make_unique<TemporaryActivationRecord>(
                identity,
                index);
            ActivationParentOptions options;
            options.ClientInstanceId = ActivationClientInstanceId(index);
            options.StartEventName = startName;
            options.LaunchRecord = record->Path();
            auto launched = LaunchActivationParent(identity, std::move(options));
            VERIFY_IS_TRUE(launched.Succeeded());
            if (!launched.Succeeded())
            {
                return;
            }
            records.emplace_back(std::move(record));
            parents.emplace_back(std::move(launched.Process));
        }
        SetEvent(start.Get());

        for (auto& parent : parents)
        {
            if (!_waitParent(parent))
            {
                return;
            }
        }
        auto probe = RequireActiveRuntime(
            identity,
            ActivationClientInstanceId(102));
        ActiveRuntimeProcessGuard runtime{ probe.ProcessId };
        VERIFY_IS_TRUE(static_cast<bool>(runtime));
        size_t launchCalls{};
        size_t startedLaunches{};
        for (const auto& recordFile : records)
        {
            const auto record = recordFile->Read();
            VERIFY_IS_TRUE(record.has_value());
            if (!record)
            {
                return;
            }
            launchCalls += record->Calls;
            startedLaunches +=
                record->Status == static_cast<int>(RuntimeProcessLaunchStatus::Started) ?
                    1 :
                    0;
        }
        VERIFY_ARE_EQUAL(size_t{ 1 }, launchCalls);
        VERIFY_ARE_EQUAL(size_t{ 1 }, startedLaunches);

        _verifyRuntimeStatus(probe);
        probe.Client.Channel.Close();
        VERIFY_IS_TRUE(runtime.TerminateAndWait());
    }

    void RuntimeOnDemandActivationTests::CrashedRuntimeRestartsWithNewProcessIdentity()
    {
        ProductRuntimeTestIdentity identity;
        auto firstParent = LaunchActivationParent(
            identity,
            { ActivationClientInstanceId(0) });
        VERIFY_IS_TRUE(firstParent.Succeeded());
        if (!firstParent.Succeeded() || !_waitParent(firstParent.Process))
        {
            return;
        }
        auto first = RequireActiveRuntime(
            identity,
            ActivationClientInstanceId(103));
        const auto firstInstance = first.Client.Handshake.RuntimeInstanceId;
        ActiveRuntimeProcessGuard firstRuntime{ first.ProcessId };
        VERIFY_IS_TRUE(static_cast<bool>(firstRuntime));
        first.Client.Channel.Close();
        VERIFY_IS_TRUE(firstRuntime.TerminateAndWait(0xe000d2d1));

        auto secondParent = LaunchActivationParent(
            identity,
            { ActivationClientInstanceId(1) });
        VERIFY_IS_TRUE(secondParent.Succeeded());
        if (!secondParent.Succeeded() || !_waitParent(secondParent.Process))
        {
            return;
        }
        auto second = RequireActiveRuntime(
            identity,
            ActivationClientInstanceId(104));
        ActiveRuntimeProcessGuard secondRuntime{ second.ProcessId };
        VERIFY_IS_TRUE(static_cast<bool>(secondRuntime));
        VERIFY_ARE_NOT_EQUAL(first.ProcessId, second.ProcessId);
        VERIFY_IS_TRUE(firstInstance != second.Client.Handshake.RuntimeInstanceId);
        _verifyRuntimeStatus(second);
        second.Client.Channel.Close();
        VERIFY_IS_TRUE(secondRuntime.TerminateAndWait());
    }
}
