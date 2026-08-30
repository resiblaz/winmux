// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "StateTransition.h"
#include "TaskRecords.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskCore
{
    inline constexpr size_t MaximumTaskEventQueryCount{ 1000 };

    enum class TaskStoreCommandStatus
    {
        Succeeded,
        Replayed,
        NotReady,
        NotFound,
        VersionConflict,
        IdempotencyConflict,
        InvalidCommand,
        InvalidRecord,
        InvalidTransition,
        ActiveRunExists,
        StorageError,
    };

    struct TaskStoreCommandResult
    {
        TaskStoreCommandStatus Status{ TaskStoreCommandStatus::StorageError };
        uint64_t CommittedTaskVersion{};
        uint64_t LastEventSequence{};
        TransitionError Transition{ TransitionError::None };
        std::string ResultJson;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == TaskStoreCommandStatus::Succeeded || Status == TaskStoreCommandStatus::Replayed;
        }
    };

    struct TaskAggregateSnapshot
    {
        TaskRecord Task;
        std::vector<ExecutionContextRecord> ExecutionContexts;
        TaskTerminalRecord Terminal;
        std::vector<AgentSessionRecord> AgentSessions;
        std::vector<TaskRunRecord> Runs;
        std::vector<TaskMessageRecord> Messages;
        uint64_t LastEventSequence{};
    };
}
