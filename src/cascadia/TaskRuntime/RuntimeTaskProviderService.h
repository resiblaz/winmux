// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProviderCoordinator.h"

#include <wil/resource.h>

#include <memory>
#include <mutex>
#include <thread>

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeTaskProviderService final
    {
    public:
        RuntimeTaskProviderService(
            Microsoft::Terminal::TaskProvider::TaskProviderAdapterFactory adapterFactory,
            std::shared_ptr<Microsoft::Terminal::TaskProvider::ITaskProviderCoordinatorSink> sink,
            Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorConfiguration configuration = {});
        ~RuntimeTaskProviderService();

        RuntimeTaskProviderService(const RuntimeTaskProviderService&) = delete;
        RuntimeTaskProviderService& operator=(const RuntimeTaskProviderService&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] bool ProviderRunning() const noexcept;
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorResult StartSession(
            const Microsoft::Terminal::TaskProvider::TaskProviderSessionRequest& request,
            HANDLE cancellationEvent);
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorResult ResumeSession(
            std::string_view taskId,
            std::string_view providerSessionId,
            HANDLE cancellationEvent);
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorResult StartRun(
            std::string_view taskId,
            const Microsoft::Terminal::TaskProvider::TaskProviderRunRequest& request,
            HANDLE cancellationEvent);
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorResult SteerRun(
            std::string_view taskId,
            std::string_view taskRunId,
            std::string_view message,
            HANDLE cancellationEvent);
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorResult InterruptRun(
            std::string_view taskId,
            std::string_view taskRunId,
            HANDLE cancellationEvent);
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorResult CloseTask(
            std::string_view taskId,
            HANDLE cancellationEvent);
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorResult SuspendTask(
            std::string_view taskId,
            HANDLE cancellationEvent);
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorResult RespondToApproval(
            std::string_view taskId,
            std::string_view taskRunId,
            std::string_view approvalId,
            Microsoft::Terminal::TaskProvider::TaskProviderApprovalDecision decision,
            HANDLE cancellationEvent);
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderCoordinatorResult RestartProvider(
            HANDLE cancellationEvent);
        void Stop() noexcept;

    private:
        mutable std::mutex _mutex;
        std::shared_ptr<Microsoft::Terminal::TaskProvider::TaskProviderCoordinator> _coordinator;
        wil::unique_handle _stopEvent;
        std::thread _eventPump;
        bool _stopping{};

        [[nodiscard]] bool _ensureEventPump() noexcept;
        void _runEventPump() noexcept;
    };
}
