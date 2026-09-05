// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeTaskOrchestrator.h"

#include "DirectDirectoryLease.h"

#include <condition_variable>
#include <functional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <wil/resource.h>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeTaskOrchestrator::Impl
    {
        struct Worker
        {
            std::thread Thread;
            std::atomic_bool Finished{};
            wil::unique_handle Cancellation{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        };

        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> Store;
        std::shared_ptr<RuntimeTaskProviderService> Provider;
        std::shared_ptr<RuntimeRtaskCommandService> Rtask;
        std::shared_ptr<RuntimeTaskTerminalService> Terminals;
        std::filesystem::path RuntimeDataRoot;
        std::filesystem::path GitExecutable;
        std::filesystem::path RtaskExecutable;
        std::string RtaskEndpoint;
        RuntimeTaskOrchestratorConfiguration Configuration;
        Microsoft::Terminal::TaskExecution::DirectDirectoryLeaseManager DirectLeaseManager;
        wil::unique_handle StopEvent;

        mutable std::mutex Mutex;
        std::condition_variable Changed;
        std::thread Scheduler;
        std::unordered_map<std::string, std::shared_ptr<Worker>> Workers;
        std::unordered_map<std::string, std::shared_ptr<Worker>> LifecycleWorkers;
        std::unordered_map<std::string, std::string> KnownRuns;
        std::unordered_set<std::string> RecoveringPreparations;
        std::unordered_map<std::string, uint64_t> TerminalIdleDeadlines;
        uint64_t LastMaintenance{};
        bool Started{};
        bool Stopping{};
        bool WakeRequested{};

        Impl(
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
            std::shared_ptr<RuntimeTaskProviderService> provider,
            std::shared_ptr<RuntimeRtaskCommandService> rtask,
            std::shared_ptr<RuntimeTaskTerminalService> terminals,
            std::filesystem::path runtimeDataRoot,
            std::filesystem::path gitExecutable,
            std::filesystem::path rtaskExecutable,
            std::string rtaskEndpoint,
            RuntimeTaskOrchestratorConfiguration configuration);

        [[nodiscard]] bool Valid() const noexcept;
        void Run() noexcept;
        void Recover() noexcept;
        void Tick() noexcept;
        void ReapWorkers() noexcept;
        bool StartLifecycleWorker(const Microsoft::Terminal::TaskCore::TaskId& task,
                                  bool urgent, std::function<void()> operation) noexcept;
        void SuspendIdleTerminal(Microsoft::Terminal::TaskCore::TaskId task) noexcept;
        void StartWorker(
            const Microsoft::Terminal::TaskCore::TaskId& task,
            const Microsoft::Terminal::TaskCore::TaskRunId& run) noexcept;
        void ExecuteRun(
            Microsoft::Terminal::TaskCore::TaskId task,
            Microsoft::Terminal::TaskCore::TaskRunId run,
            HANDLE preparationCancellation) noexcept;
        [[nodiscard]] bool WorkerBusy(const Microsoft::Terminal::TaskCore::TaskRunId& run) noexcept;
        void CancelPreparation(const Microsoft::Terminal::TaskCore::TaskRunId& run) noexcept;
        void FinalizeRun(
            const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate,
            const Microsoft::Terminal::TaskCore::TaskRunRecord& run) noexcept;
        void ClaimMessages(const Microsoft::Terminal::TaskCore::TaskId& task) noexcept;
        void ObserveTerminalIdle(
            const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate) noexcept;
        void HandleIntent(const Microsoft::Terminal::TaskCore::TaskIntentRecord& intent) noexcept;
        [[nodiscard]] bool AdvanceIntent(
            const Microsoft::Terminal::TaskCore::TaskId& task,
            const Microsoft::Terminal::TaskCore::TaskIntentId& intent,
            Microsoft::Terminal::TaskCore::TaskIntentStage stage,
            std::string reason = {},
            std::string detailJson = "{}") noexcept;
        [[nodiscard]] bool TransitionRun(
            const Microsoft::Terminal::TaskCore::TaskId& task,
            const Microsoft::Terminal::TaskCore::TaskRunId& run,
            Microsoft::Terminal::TaskCore::TaskRunState state,
            std::string reason = {}) noexcept;
        [[nodiscard]] bool TransitionTerminal(
            const Microsoft::Terminal::TaskCore::TaskId& task,
            Microsoft::Terminal::TaskCore::TaskTerminalState state,
            uint64_t generation,
            bool explicitOpen = false) noexcept;
        [[nodiscard]] bool ChangeStatus(
            const Microsoft::Terminal::TaskCore::TaskId& task,
            Microsoft::Terminal::TaskCore::TaskStatus status) noexcept;
    };
}
