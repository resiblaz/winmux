// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProjectionCoordinator.h"

namespace winrt::TerminalApp::implementation::tasks
{
    TaskProjectionCoordinator::TaskProjectionCoordinator() :
        _snapshot{ std::make_shared<const std::vector<TaskProjection>>() }
    {
    }

    bool TaskProjectionCoordinator::Apply(
        std::string epoch,
        const uint64_t generation,
        std::vector<TaskProjection> tasks)
    {
        if (epoch.empty() || generation == 0 || tasks.size() > 512) return false;
        try
        {
            std::ranges::sort(tasks, [](const auto& left, const auto& right) {
                return left.UpdatedAtUnixMilliseconds != right.UpdatedAtUnixMilliseconds ?
                           left.UpdatedAtUnixMilliseconds > right.UpdatedAtUnixMilliseconds : left.Id < right.Id;
            });
            auto immutable = std::make_shared<const std::vector<TaskProjection>>(std::move(tasks));
            const std::scoped_lock lock{ _mutex };
            if (epoch == _epoch && generation <= _generation) return false;
            _epoch = std::move(epoch);
            _generation = generation;
            _snapshot = std::move(immutable);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void TaskProjectionCoordinator::UpsertOptimistic(TaskProjection task)
    {
        try
        {
            const std::scoped_lock lock{ _mutex };
            auto next = *_snapshot;
            const auto found = std::ranges::find(next, task.Id, &TaskProjection::Id);
            if (found == next.end()) next.insert(next.begin(), std::move(task));
            else *found = std::move(task);
            _snapshot = std::make_shared<const std::vector<TaskProjection>>(std::move(next));
        }
        catch (...)
        {
        }
    }

    bool TaskProjectionCoordinator::Merge(
        const std::string_view epoch,
        const uint64_t generation,
        TaskProjection task)
    {
        try
        {
            const std::scoped_lock lock{ _mutex };
            if (epoch.empty() || epoch != _epoch || generation < _generation) return false;
            auto next = *_snapshot;
            const auto found = std::ranges::find(next, task.Id, &TaskProjection::Id);
            if (found == next.end()) next.emplace_back(std::move(task));
            else *found = std::move(task);
            _generation = generation;
            _snapshot = std::make_shared<const std::vector<TaskProjection>>(std::move(next));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::vector<TaskProjection> TaskProjectionCoordinator::Snapshot() const
    {
        const std::scoped_lock lock{ _mutex };
        return *_snapshot;
    }

    uint64_t TaskProjectionCoordinator::Generation() const noexcept
    {
        const std::scoped_lock lock{ _mutex };
        return _generation;
    }

    size_t TaskProjectionCoordinator::Size() const noexcept
    {
        const std::scoped_lock lock{ _mutex };
        return _snapshot->size();
    }
}
