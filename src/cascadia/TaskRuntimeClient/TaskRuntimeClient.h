// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientInterfaces.h"
#include "RuntimeClientSubscriptionStream.h"
#include "TaskRuntimeClientTypes.h"

#include "RuntimeControlConnector.h"

#include <memory>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeClientSubscriptionHandle
    {
        bool Accepted{};
        RuntimeClientLogicalSubscriptionId LogicalId{};
        RuntimeClientSubscriptionRevision Revision{};
        std::shared_ptr<RuntimeClientSubscriptionStream> Stream;
    };

    struct TaskRuntimeClientDependencies
    {
        std::shared_ptr<IRuntimeControlConnector> Connector;
        std::shared_ptr<IRuntimeClientRequestIdSource> RequestIds;
        std::shared_ptr<IRuntimeClientBackoffPolicy> Backoff;
    };

    class TaskRuntimeClient final
    {
    public:
        explicit TaskRuntimeClient(
            TaskRuntimeClientConfiguration configuration,
            TaskRuntimeClientDependencies dependencies = {});
        ~TaskRuntimeClient();

        TaskRuntimeClient(const TaskRuntimeClient&) = delete;
        TaskRuntimeClient& operator=(const TaskRuntimeClient&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] bool Start();
        [[nodiscard]] RuntimeClientRequestHandle Submit(
            Microsoft::Terminal::TaskRuntimeProtocol::ControlMethod method,
            std::string payloadJson = "{}",
            uint32_t maximumAttempts = 0);
        [[nodiscard]] RuntimeClientSubscriptionHandle Subscribe(
            RuntimeClientSubscribeOptions options = {});
        void RequestStop() noexcept;
        [[nodiscard]] bool WaitForStopped() noexcept;

        [[nodiscard]] TaskRuntimeClientState State() const noexcept;
        [[nodiscard]] RuntimeClientEpoch ActiveEpoch() const noexcept;
        [[nodiscard]] std::string RuntimeInstanceId() const;

    private:
        struct Impl;
        std::shared_ptr<Impl> _impl;
    };
}
