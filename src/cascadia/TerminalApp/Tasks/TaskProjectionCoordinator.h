// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProjection.h"

#include <memory>
#include <mutex>

namespace winrt::TerminalApp::implementation::tasks
{
    class TaskProjectionCoordinator final
    {
    public:
        TaskProjectionCoordinator();

        [[nodiscard]] bool Apply(std::string epoch, uint64_t generation, std::vector<TaskProjection> tasks);
        [[nodiscard]] bool Merge(std::string_view epoch, uint64_t generation, TaskProjection task);
        void UpsertOptimistic(TaskProjection task);
        [[nodiscard]] std::vector<TaskProjection> Snapshot() const;
        [[nodiscard]] uint64_t Generation() const noexcept;
        [[nodiscard]] size_t Size() const noexcept;

    private:
        mutable std::mutex _mutex;
        std::shared_ptr<const std::vector<TaskProjection>> _snapshot;
        uint64_t _generation{};
        std::string _epoch;
    };
}
