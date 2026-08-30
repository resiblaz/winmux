// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeActivationServices.h"
#include "RuntimeLaunchLockProvider.h"
#include "RuntimeProcessLauncher.h"

#include "RuntimeControlConnector.h"

#include <filesystem>
#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeLaunchingConnectorConfiguration
    {
        std::filesystem::path RuntimeExecutable;
        std::filesystem::path TrustedRoot;
        uint32_t RetryPollMilliseconds{ 50 };
    };

    struct RuntimeLaunchingConnectorDependencies
    {
        std::shared_ptr<IRuntimeControlConnector> DirectConnector;
        std::shared_ptr<IRuntimeLaunchLockProvider> LaunchLocks;
        std::shared_ptr<IRuntimeProcessLauncher> ProcessLauncher;
        std::shared_ptr<IRuntimeActivationServices> Services;
    };

    class RuntimeLaunchingControlConnector final : public IRuntimeControlConnector
    {
    public:
        RuntimeLaunchingControlConnector(
            RuntimeLaunchingConnectorConfiguration configuration,
            RuntimeLaunchingConnectorDependencies dependencies) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeControlConnectionResult Connect(
            const RuntimeIdentity& identity,
            const Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest& request,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr) noexcept override;

    private:
        RuntimeLaunchingConnectorConfiguration _configuration;
        RuntimeLaunchingConnectorDependencies _dependencies;
    };

    [[nodiscard]] std::shared_ptr<IRuntimeControlConnector>
        MakeRuntimeLaunchingControlConnector(
            std::filesystem::path runtimeExecutable,
            std::filesystem::path trustedRoot);
}
