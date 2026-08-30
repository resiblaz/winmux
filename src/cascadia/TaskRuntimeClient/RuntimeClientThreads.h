// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <functional>
#include <memory>
#include <thread>

namespace Microsoft::Terminal::TaskRuntime::details
{
    struct RuntimeClientEpochContext;

    void DetachOrRetainRuntimeClientThread(
        std::unique_ptr<std::thread>& thread) noexcept;
    [[nodiscard]] bool WaitAndReleaseRuntimeClientThread(
        std::unique_ptr<std::thread>& thread) noexcept;
    [[nodiscard]] std::unique_ptr<std::thread> StartRuntimeClientEpochWorker(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        std::function<void()> callback);
}
