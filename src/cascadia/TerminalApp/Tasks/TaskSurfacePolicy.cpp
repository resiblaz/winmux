// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskSurfacePolicy.h"

namespace winrt::TerminalApp::implementation::tasks
{
    bool TaskSurfacePolicy::Register(
        const TaskSurfaceId surface,
        TaskSurfaceOwnership ownership)
    {
        if (surface == 0 || ownership.TaskId.empty() || ownership.Generation == 0 || Find(ownership.TaskId))
        {
            return false;
        }
        try
        {
            return _entries.emplace(surface, Entry{ std::move(ownership), std::nullopt }).second;
        }
        catch (...)
        {
            return false;
        }
    }

    void TaskSurfacePolicy::Unregister(const TaskSurfaceId surface) noexcept
    {
        _entries.erase(surface);
    }

    bool TaskSurfacePolicy::IsTaskOwned(const TaskSurfaceId surface) const noexcept
    {
        return _entries.contains(surface);
    }

    std::optional<TaskSurfaceId> TaskSurfacePolicy::Find(const std::wstring_view taskId) const noexcept
    try
    {
        const auto found = std::ranges::find_if(_entries, [&](const auto& value) {
            return value.second.Ownership.TaskId == taskId;
        });
        return found == _entries.end() ? std::nullopt : std::optional<TaskSurfaceId>{ found->first };
    }
    catch (...)
    {
        return std::nullopt;
    }

    bool TaskSurfacePolicy::AuthorizeClose(
        const std::wstring_view taskId,
        const uint64_t generation,
        const TaskSurfaceCloseAuthorization authorization) noexcept
    {
        if (taskId.empty() || generation == 0 || authorization == 0)
        {
            return false;
        }
        const auto surface = Find(taskId);
        if (!surface)
        {
            return false;
        }
        const auto found = _entries.find(*surface);
        if (found == _entries.end() || found->second.Ownership.Generation != generation)
        {
            return false;
        }
        found->second.Authorization = authorization;
        return true;
    }

    TaskSurfaceCloseDecision TaskSurfacePolicy::Evaluate(
        const TaskSurfaceId surface,
        const TaskSurfaceCloseOrigin origin,
        const std::optional<TaskSurfaceCloseAuthorization> authorization) noexcept
    {
        const auto found = _entries.find(surface);
        if (found == _entries.end())
        {
            return { TaskSurfaceCloseAction::Allow, {}, false };
        }
        TaskSurfaceCloseDecision result;
        result.TaskId = found->second.Ownership.TaskId;
        switch (origin)
        {
        case TaskSurfaceCloseOrigin::WindowExit:
            result.Action = TaskSurfaceCloseAction::Detach;
            return result;
        case TaskSurfaceCloseOrigin::ConnectionExit:
            result.Action = TaskSurfaceCloseAction::RetainPlaceholder;
            return result;
        case TaskSurfaceCloseOrigin::CloseOtherTabs:
        case TaskSurfaceCloseOrigin::CloseTabsAfter:
            result.Action = TaskSurfaceCloseAction::Skip;
            return result;
        case TaskSurfaceCloseOrigin::TaskDetail:
        case TaskSurfaceCloseOrigin::RuntimeEndOfStream:
            if (authorization && found->second.Authorization == authorization)
            {
                found->second.Authorization.reset();
                result.Action = TaskSurfaceCloseAction::Allow;
                result.AuthorizationConsumed = true;
                return result;
            }
            break;
        default:
            break;
        }
        result.Action = TaskSurfaceCloseAction::Deny;
        return result;
    }
}
