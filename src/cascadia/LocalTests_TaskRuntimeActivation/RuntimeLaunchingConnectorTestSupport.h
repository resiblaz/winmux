// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ActivationTestSupport.h"
#include "FakeRuntimeActivationServices.h"
#include "FakeRuntimeLaunchingConnector.h"
#include "FakeRuntimeWindowsProcessApi.h"
#include "RuntimeLaunchingConnector.h"
#include "RuntimeProcessLauncherTestSupport.h"

namespace TaskRuntimeActivationLocalTests
{
    inline constexpr std::string_view LaunchingClientId{
        "81000000-0000-4000-8000-000000000001"
    };

    inline RuntimeLaunchingConnectorConfiguration LaunchingConfiguration()
    {
        return {
            LR"(C:\Program Files\Richer Terminal\TaskRuntime.exe)",
            LR"(C:\Program Files\Richer Terminal)",
            50,
        };
    }

    inline HelloRequest LaunchingHello(const RuntimeIdentity& identity)
    {
        HelloRequest request;
        request.ClientInstanceId = std::string{ LaunchingClientId };
        request.ClientKind = ProtocolClientKind::TestPeer;
        request.ExpectedRuntimeIdentity = identity.ProtocolIdentity;
        return request;
    }

    struct LaunchingFixture
    {
        UniqueActivationIdentity Identity;
        std::shared_ptr<FakeDirectConnector> Direct{ std::make_shared<FakeDirectConnector>() };
        std::shared_ptr<FakeLaunchLockProvider> Locks{
            std::make_shared<FakeLaunchLockProvider>()
        };
        std::shared_ptr<FakeProcessLauncher> Launcher{
            std::make_shared<FakeProcessLauncher>()
        };
        std::shared_ptr<FakeRuntimeActivationServices> Services{
            std::make_shared<FakeRuntimeActivationServices>(100)
        };

        RuntimeLaunchingControlConnector Connector{
            LaunchingConfiguration(),
            {
                Direct,
                Locks,
                Launcher,
                Services,
            },
        };

        [[nodiscard]] RuntimeControlConnectionResult Connect(
            const uint32_t timeoutMilliseconds = 1000,
            const HANDLE cancellationEvent = nullptr)
        {
            return Connector.Connect(
                Identity.Identity(),
                LaunchingHello(Identity.Identity()),
                timeoutMilliseconds,
                cancellationEvent);
        }
    };

    inline std::shared_ptr<FakeRuntimeWindowsProcessApi> ConfigureSuccessfulProcessLauncher(
        LaunchingFixture& fixture)
    {
        auto windows = std::make_shared<FakeRuntimeWindowsProcessApi>();
        const auto launcher = MakeTestProcessLauncher(windows);
        fixture.Launcher->Handler = [launcher](const RuntimeProcessLaunchRequest& request) {
            return launcher->Launch(request);
        };
        return windows;
    }
}
