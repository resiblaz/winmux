// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProjection.h"

#include <memory>
#include <optional>
#include <functional>

namespace winrt::TerminalApp::implementation::agents { class IAgentCatalogSource; }

namespace winrt::TerminalApp::implementation::tasks
{
    class ITaskProjectionSource
    {
    public:
        virtual ~ITaskProjectionSource() = default;
        virtual std::shared_ptr<agents::IAgentCatalogSource> AgentCatalog() const { return nullptr; }

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
        [[nodiscard]] virtual std::wstring ResolveWorkspaceKey(std::wstring_view workspaceId,
                                                               std::wstring_view,
                                                               std::wstring_view) const
        {
            return std::wstring{ workspaceId };
        }
    };

    [[nodiscard]] std::shared_ptr<ITaskProjectionSource> MakePrototypeTaskProjectionSource();
}
