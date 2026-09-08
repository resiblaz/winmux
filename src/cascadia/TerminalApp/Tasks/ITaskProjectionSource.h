// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProjection.h"

#include <memory>
#include <optional>
#include <functional>

namespace winrt::TerminalApp::implementation::agents { class IAgentCatalogSource; }
namespace winrt::TerminalApp::implementation::projects { class IWorkspaceSource; }

namespace winrt::TerminalApp::implementation::tasks
{
    class ITaskProjectionSource
    {
    public:
        virtual ~ITaskProjectionSource() = default;
        virtual std::shared_ptr<agents::IAgentCatalogSource> AgentCatalog() const { return nullptr; }
        virtual std::shared_ptr<projects::IWorkspaceSource> WorkspaceSource() const { return nullptr; }

        using ChangedHandler = std::function<void()>;
        using SurfaceHandler = std::function<void(const TaskSurfaceDescriptor&)>;

        [[nodiscard]] virtual std::vector<TaskProjection> Snapshot() const = 0;
        [[nodiscard]] virtual TaskProjection CreateTask(const TaskDraft& draft, bool startImmediately) = 0;
        virtual void CreateTaskAsync(TaskCreationAttempt attempt, std::function<void(TaskCreationResult)> completed)
        {
            auto task = CreateTask(attempt.Draft, attempt.StartImmediately);
            completed({ !task.Id.empty(), false, std::move(task), {} });
        }
        [[nodiscard]] virtual std::optional<TaskProjection> Execute(const std::wstring& taskId, TaskCommand command) = 0;
        [[nodiscard]] virtual bool QueueMessage(const std::wstring&, const std::wstring&) { return false; }
        [[nodiscard]] virtual bool ReviseMessage(
            const std::wstring&,
            const std::wstring&,
            uint64_t,
            const std::optional<std::wstring>&) { return false; }
        [[nodiscard]] virtual bool IsPrototype() const noexcept { return false; }
        [[nodiscard]] virtual bool IsLoading() const noexcept { return false; }
        virtual void SetChangedHandler(ChangedHandler) {}
        virtual void SetSurfaceHandler(SurfaceHandler) {}
        [[nodiscard]] virtual bool Start() { return true; }
        virtual void Stop() noexcept {}
        virtual void SelectTask(std::wstring_view) {}
        virtual void ConfigureQuery(const TaskListQuery&) {}
        virtual void RequestRefresh() {}
        virtual void LoadMore(std::optional<TaskStatus> = std::nullopt) {}
        virtual bool UsesSummaries() const noexcept { return false; }
        virtual bool HasMoreTasks(std::optional<TaskStatus> = std::nullopt) const noexcept { return false; }
        virtual uint64_t QueryTotal() const noexcept { return 0; }
        [[nodiscard]] virtual std::wstring ResolveWorkspaceKey(std::wstring_view workspaceId,
                                                               std::wstring_view,
                                                               std::wstring_view) const
        {
            return std::wstring{ workspaceId };
        }
        virtual std::array<uint64_t, 7> QueryCounts() const noexcept { return {}; }
    };

    [[nodiscard]] std::shared_ptr<ITaskProjectionSource> MakePrototypeTaskProjectionSource();
}
