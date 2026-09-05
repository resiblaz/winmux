// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskProviderAdapter.h"

#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <type_traits>
#include <utility>
#include <wil/resource.h>

namespace Microsoft::Terminal::TaskProvider
{
    struct TaskProviderSessionBinding
    {
        std::string TaskId;
        std::string ProviderSessionId;
        TaskProviderSessionState State{ TaskProviderSessionState::Starting };
    };

    struct TaskProviderRunBinding
    {
        std::string TaskId;
        std::string TaskRunId;
        std::string ProviderSessionId;
        std::string ProviderRunId;
        TaskProviderRunState State{ TaskProviderRunState::Starting };
    };

    class ITaskProviderCoordinatorSink
    {
    public:
        virtual ~ITaskProviderCoordinatorSink() = default;
        [[nodiscard]] virtual TaskProviderResult OnSessionStateChanged(const TaskProviderSessionBinding& session) = 0;
        [[nodiscard]] virtual TaskProviderResult OnRunStateChanged(
            const TaskProviderRunBinding& run,
            const std::optional<TaskProviderFailureDisposition>& failure) = 0;
        [[nodiscard]] virtual TaskProviderResult OnEvent(
            const TaskProviderSessionBinding& session,
            const std::optional<TaskProviderRunBinding>& run,
            const TaskProviderEvent& event) = 0;
        [[nodiscard]] virtual TaskProviderResult OnSteeringDeferred(
            const TaskProviderRunBinding& run,
            std::string_view message,
            std::string_view reasonCode) = 0;
    };

    struct TaskProviderCoordinatorConfiguration
    {
        size_t MaximumSessions{ 256 };
        size_t MaximumRuns{ 1024 };
        uint32_t EventPollMilliseconds{ 100 };

        [[nodiscard]] bool Valid() const noexcept;
    };

    struct TaskProviderCoordinatorResult : TaskProviderResult
    {
        std::optional<TaskProviderSessionBinding> Session;
        std::optional<TaskProviderRunBinding> Run;
        bool Deferred{};
        bool ClosePending{};
        std::vector<std::string> AffectedTaskIds;
    };

    using TaskProviderAdapterFactory = std::function<std::unique_ptr<ITaskProviderAdapter>()>;

    class TaskProviderCoordinator final
    {
    public:
        TaskProviderCoordinator(
            TaskProviderAdapterFactory adapterFactory,
            std::shared_ptr<ITaskProviderCoordinatorSink> sink,
            TaskProviderCoordinatorConfiguration configuration = {});
        ~TaskProviderCoordinator();

        TaskProviderCoordinator(const TaskProviderCoordinator&) = delete;
        TaskProviderCoordinator& operator=(const TaskProviderCoordinator&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] bool ProviderRunning() const noexcept;
        [[nodiscard]] TaskProviderCoordinatorResult StartSession(
            const TaskProviderSessionRequest& request,
            HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderCoordinatorResult ResumeSession(
            std::string_view taskId,
            std::string_view providerSessionId,
            HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderCoordinatorResult StartRun(
            std::string_view taskId,
            const TaskProviderRunRequest& request,
            HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderCoordinatorResult SteerRun(
            std::string_view taskId,
            std::string_view taskRunId,
            std::string_view message,
            HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderCoordinatorResult InterruptRun(
            std::string_view taskId,
            std::string_view taskRunId,
            HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderCoordinatorResult CloseTask(
            std::string_view taskId,
            HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderCoordinatorResult SuspendTask(
            std::string_view taskId,
            HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderCoordinatorResult RespondToApproval(
            std::string_view taskId,
            std::string_view taskRunId,
            std::string_view approvalId,
            TaskProviderApprovalDecision decision,
            HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderCoordinatorResult PumpOneEvent(HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderCoordinatorResult RestartProvider(HANDLE cancellationEvent);
        [[nodiscard]] std::vector<TaskProviderSessionBinding> Sessions() const;
        [[nodiscard]] std::vector<TaskProviderRunBinding> Runs() const;
        void Stop() noexcept;

    private:
        TaskProviderAdapterFactory _factory;
        std::shared_ptr<ITaskProviderCoordinatorSink> _sink;
        TaskProviderCoordinatorConfiguration _configuration;
        mutable std::mutex _mutex;
        std::mutex _initializationMutex;
        std::mutex _eventMutex;
        std::shared_ptr<ITaskProviderAdapter> _adapter;
        std::unordered_map<std::string, TaskProviderSessionBinding> _sessionsByTask;
        std::unordered_map<std::string, std::string> _tasksByProviderSession;
        std::unordered_map<std::string, TaskProviderRunBinding> _runsByTaskRun;
        std::unordered_map<std::string, std::string> _taskRunsByProviderRun;
        std::unordered_map<std::string, std::weak_ptr<std::mutex>> _taskGates;
        struct DeferredEvent
        {
            std::shared_ptr<ITaskProviderAdapter> Adapter;
            std::shared_ptr<std::mutex> Gate;
            TaskProviderEvent Event;
        };
        std::deque<DeferredEvent> _deferredEvents;
        size_t _startingSessions{};
        size_t _startingRuns{};
        uint64_t _epoch{};
        bool _valid{};

        [[nodiscard]] std::shared_ptr<std::mutex> _taskGate(std::string_view taskId);
        [[nodiscard]] TaskProviderResult _ensureProvider(std::unique_lock<std::mutex>& lock, HANDLE cancellationEvent);
        template<typename TCall>
        auto _invoke(std::unique_lock<std::mutex>& lock, TCall&& call)
            -> std::optional<std::invoke_result_t<TCall, ITaskProviderAdapter&>>
        {
            using Result = std::invoke_result_t<TCall, ITaskProviderAdapter&>;
            const auto adapter = _adapter;
            if (!adapter) return std::nullopt;
            std::optional<Result> result;
            lock.unlock();
            {
                const auto relock = wil::scope_exit([&] { lock.lock(); });
                try { result = std::invoke(std::forward<TCall>(call), *adapter); }
                catch (...) { result.emplace(); result->Status = TaskProviderStatus::ResourceFailure; }
            }
            // Never publish a completion from an adapter replaced during IO.
            return _adapter == adapter ? std::move(result) : std::nullopt;
        }
        [[nodiscard]] TaskProviderCoordinatorResult _reconcileProviderFailure(
            std::unique_lock<std::mutex>& lock,
            TaskProviderStatus status,
            std::string_view providerCode,
            std::string error);
        [[nodiscard]] TaskProviderRunBinding* _findRun(
            std::string_view taskId,
            std::string_view taskRunId) noexcept;
        [[nodiscard]] TaskProviderRunBinding* _activeRun(std::string_view taskId) noexcept;
        [[nodiscard]] TaskProviderResult _publishSession(const TaskProviderSessionBinding& session) const;
        [[nodiscard]] TaskProviderResult _publishRun(
            const TaskProviderRunBinding& run,
            const std::optional<TaskProviderFailureDisposition>& failure = std::nullopt) const;
    };
}
