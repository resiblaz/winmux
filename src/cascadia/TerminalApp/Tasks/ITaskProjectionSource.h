// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProjection.h"

#include <memory>
#include <optional>

namespace winrt::TerminalApp::implementation::tasks
{
    class ITaskProjectionSource
    {
    public:
        virtual ~ITaskProjectionSource() = default;

        [[nodiscard]] virtual std::vector<TaskProjection> Snapshot() const = 0;
        [[nodiscard]] virtual TaskProjection CreateTask(const TaskDraft& draft, bool startImmediately) = 0;
        [[nodiscard]] virtual std::optional<TaskProjection> Execute(const std::wstring& taskId, TaskCommand command) = 0;
        [[nodiscard]] virtual bool IsPrototype() const noexcept { return false; }
        [[nodiscard]] virtual std::wstring ResolveWorkspaceKey(std::wstring_view workspaceId,
                                                               std::wstring_view,
                                                               std::wstring_view) const
        {
            return std::wstring{ workspaceId };
        }
    };

    [[nodiscard]] std::shared_ptr<ITaskProjectionSource> MakePrototypeTaskProjectionSource();
}
