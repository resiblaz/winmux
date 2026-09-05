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
    class RuntimeTaskProjectionSource final : public ITaskProjectionSource
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
        Windows::UI::Core::CoreDispatcher _dispatcher{ nullptr };
        TaskProjectionCoordinator _coordinator;
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
        std::shared_ptr<TaskAgentSessionRegistry> _agentSessions;

        [[nodiscard]] bool _enqueue(WorkItem item);
        void _run() noexcept;
        void _refresh(const std::optional<std::string>& taskId = std::nullopt) noexcept;
        void _submit(const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& command) noexcept;
        void _notify() noexcept;
        void _notifySurface(const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandRequest& command,
                            const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskUiCommandResult& result) noexcept;
    };

    [[nodiscard]] std::shared_ptr<ITaskProjectionSource> MakeRuntimeTaskProjectionSource(
        std::shared_ptr<TaskAgentSessionRegistry> agentSessions = nullptr);
}
