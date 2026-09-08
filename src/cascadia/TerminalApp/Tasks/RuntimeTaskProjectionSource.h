// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskProjectionSource.h"
#include "TaskProjectionCoordinator.h"
#include "TaskAgentSessionRegistry.h"

#include "TaskRuntimeClient.h"
#include "TaskProjectionRpc.h"

#include <condition_variable>
#include <deque>
#include <thread>

namespace winrt::TerminalApp::implementation::tasks
{
    class RuntimeTaskProjectionSource final : public ITaskProjectionSource, public std::enable_shared_from_this<RuntimeTaskProjectionSource>
    {
    public:
        RuntimeTaskProjectionSource(
            ::Microsoft::Terminal::TaskRuntime::TaskRuntimeClientConfiguration configuration,
            Windows::UI::Core::CoreDispatcher dispatcher,
            std::shared_ptr<TaskAgentSessionRegistry> agentSessions = nullptr,
            ::Microsoft::Terminal::TaskRuntime::TaskRuntimeClientDependencies dependencies = {});
        ~RuntimeTaskProjectionSource() override;

        [[nodiscard]] std::vector<TaskProjection> Snapshot() const override;
        [[nodiscard]] TaskProjection CreateTask(const TaskDraft& draft, bool startImmediately) override;
        void CreateTaskAsync(TaskCreationAttempt attempt, std::function<void(TaskCreationResult)> completed) override;
        std::shared_ptr<agents::IAgentCatalogSource> AgentCatalog() const override { return _agentCatalog; }
        std::shared_ptr<projects::IWorkspaceSource> WorkspaceSource() const override { return _workspaceSource; }
        [[nodiscard]] std::optional<TaskProjection> Execute(const std::wstring& taskId, TaskCommand command) override;
        [[nodiscard]] bool QueueMessage(const std::wstring& taskId, const std::wstring& message) override;
        [[nodiscard]] bool ReviseMessage(
            const std::wstring& taskId,
            const std::wstring& messageId,
            uint64_t revision,
            const std::optional<std::wstring>& replacement) override;
        void SetChangedHandler(ChangedHandler handler) override;
        void SetSurfaceHandler(SurfaceHandler handler) override;
        [[nodiscard]] bool Start() override;
        [[nodiscard]] bool IsLoading() const noexcept override;
        void Stop() noexcept override;
        void SelectTask(std::wstring_view taskId) override;
        void ConfigureQuery(const TaskListQuery& query) override;
        void RequestRefresh() override { static_cast<void>(_enqueue({ WorkKind::Refresh, std::nullopt, std::nullopt })); }
        void LoadMore(std::optional<TaskStatus> status = std::nullopt) override;
        bool UsesSummaries() const noexcept override;
        bool HasMoreTasks(std::optional<TaskStatus> status = std::nullopt) const noexcept override;
        uint64_t QueryTotal() const noexcept override;
        std::array<uint64_t, 7> QueryCounts() const noexcept override;
        [[nodiscard]] std::wstring ResolveWorkspaceKey(
            std::wstring_view workspaceId,
            std::wstring_view repositoryRoot,
            std::wstring_view workingDirectory) const override;

    private:
        enum class WorkKind
        {
            Refresh,
            Command,
        };

        struct WorkItem
        {
            WorkKind Kind{ WorkKind::Refresh };
            std::optional<::Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest> Command;
            std::optional<std::string> QueryTaskId;
        };

        ::Microsoft::Terminal::TaskRuntime::RuntimeIdentity _identity;
        std::string _clientInstanceId;
        std::shared_ptr<::Microsoft::Terminal::TaskRuntime::TaskRuntimeClient> _client;
        std::shared_ptr<agents::IAgentCatalogSource> _agentCatalog;
        std::shared_ptr<projects::IWorkspaceSource> _workspaceSource;
        Windows::UI::Core::CoreDispatcher _dispatcher{ nullptr };
        TaskProjectionCoordinator _coordinator;
        std::mutex _publicationMutex;
        uint64_t _publicationRevision{};
        mutable std::mutex _mutex;
        std::condition_variable _condition;
        std::deque<WorkItem> _work;
        std::thread _worker;
        ChangedHandler _changed;
        SurfaceHandler _surfaceChanged;
        bool _started{};
        bool _stopping{};
        bool _drainSubmitted{};
        std::atomic_bool _loading{ true };
        std::string _selectedTaskId;
        TaskListQuery _listQuery;
        uint64_t _queryRevision{ 1 };
        std::array<size_t, 7> _loadedLimits{ 50, 50, 50, 50, 50, 50, 50 };
        std::array<uint64_t, 7> _queryCounts{};
        std::array<bool, 7> _hasMore{};
        std::shared_ptr<TaskAgentSessionRegistry> _agentSessions;
        uint64_t _sessionIndexAfter{};
        std::string _sessionIndexEpoch;

        [[nodiscard]] bool _enqueue(WorkItem item);
        void _run() noexcept;
        void _refresh(const std::optional<std::string>& taskId = std::nullopt) noexcept;
        void _refreshSummaries() noexcept;
        void _refreshTaskSessionIndex() noexcept;
        void _submit(const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& command) noexcept;
        void _notify() noexcept;
        void _notifySurface(const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& command,
                            const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandResult& result) noexcept;
    };

    [[nodiscard]] std::shared_ptr<ITaskProjectionSource> MakeRuntimeTaskProjectionSource(
        std::shared_ptr<TaskAgentSessionRegistry> agentSessions = nullptr);
}
