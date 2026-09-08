// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskProjectionCoordinator.h"
#include <unordered_map>

namespace winrt::TerminalApp::implementation::tasks
{
    bool TaskProjectionCoordinator::ApplySummaries(std::string epoch, std::vector<TaskProjection> tasks, std::wstring_view selectedTask)
    {
        if (epoch.empty()) return false;
        try
        {
            std::unordered_map<std::wstring, size_t> ids;
            for (size_t i = 0; i < tasks.size(); ++i)
                if (tasks[i].Id.empty() || !tasks[i].IsSummary || !ids.emplace(tasks[i].Id, i).second) return false;
            const std::scoped_lock lock{ _mutex };
            if (epoch == _epoch)
            {
                for (const auto& previous : *_snapshot)
                {
                    const auto found = ids.find(previous.Id);
                    if (found == ids.end())
                    {
                        // A filtered or unloaded row is not evidence that the selected Task was deleted.
                        if (previous.Id == selectedTask && !previous.IsSummary) tasks.push_back(previous);
                        continue;
                    }
                    auto& next = tasks[found->second];
                    if (!previous.IsSummary && (previous.Id == selectedTask ||
                        (previous.Version == next.Version && previous.LastEventSequence == next.LastEventSequence)))
                    {
                        auto full = previous;
                        full.Workspace = next.Workspace; full.BoardRank = next.BoardRank; full.ColumnRevision = next.ColumnRevision;
                        full.HasPendingIntent = next.HasPendingIntent; full.HasPendingMessages = next.HasPendingMessages;
                        full.ArchivedProject = next.ArchivedProject; full.ManualHold = next.ManualHold;
                        next = std::move(full);
                    }
                }
            }
            _epoch = std::move(epoch); _summaryMode = true;
            std::ranges::sort(tasks, [](const auto& a, const auto& b) {
                return a.UpdatedAtUnixMilliseconds != b.UpdatedAtUnixMilliseconds ? a.UpdatedAtUnixMilliseconds > b.UpdatedAtUnixMilliseconds : a.Id < b.Id;
            });
            _snapshot = std::make_shared<const std::vector<TaskProjection>>(std::move(tasks));
            return true;
        }
        catch (...) { return false; }
    }

    void TaskProjectionCoordinator::RemoveTask(std::string_view epoch, std::wstring_view taskId)
    {
        const std::scoped_lock lock{ _mutex };
        if (epoch != _epoch) return;
        auto next = *_snapshot;
        std::erase_if(next, [&](const auto& task) { return task.Id == taskId; });
        _snapshot = std::make_shared<const std::vector<TaskProjection>>(std::move(next));
    }
}
