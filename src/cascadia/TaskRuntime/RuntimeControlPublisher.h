// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlServer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeControlSession;

    [[nodiscard]] RuntimeControlPublishStatus PublishRuntimeControlNotification(
        const Microsoft::Terminal::TaskRuntimeProtocol::ControlNotification& notification,
        std::mutex& sessionsMutex,
        const std::vector<std::unique_ptr<RuntimeControlSession>>& sessions,
        std::mutex& subscriptionOwnershipMutex,
        const std::atomic_bool& stopRequested,
        const std::atomic_bool& isRunning);
}
