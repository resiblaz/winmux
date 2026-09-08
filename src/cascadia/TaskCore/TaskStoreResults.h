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
    inline constexpr size_t MaximumTaskRecoveryQueryCount{ 256 };
    inline constexpr size_t MaximumTombstoneCleanupBatch{ 100 };
    inline constexpr int64_t TaskTombstoneRetentionMilliseconds{ 30LL * 24 * 60 * 60 * 1000 };

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
        ActiveIntentExists,
        Tombstoned,
        CapacityExceeded,
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
        std::optional<TaskGitWorktreeRecord> GitWorktree;
        std::vector<TaskRunGitCheckpointRecord> GitCheckpoints;
        TaskTerminalRecord Terminal;
        std::optional<TaskIntentRecord> Intent;
        std::vector<AgentSessionRecord> AgentSessions;
        std::vector<TaskRunRecord> Runs;
        std::vector<TaskMessageRecord> Messages;
        std::vector<TaskRunMessageRecord> RunMessages;
        uint64_t LastEventSequence{};
        std::string BoardNote;
        bool BoardNoteIsBlocked{};
    };

    struct TaskRecoverySnapshot
    {
        std::vector<TaskRunRecord> ActiveRuns;
        std::vector<TaskIntentRecord> PendingIntents;
        std::vector<TaskTerminalRecord> RecoverableTerminals;
        std::vector<TaskGitWorktreeRecord> RecoverableGitWorktrees;
        std::vector<TaskRunGitCheckpointRecord> RecoverableGitCheckpoints;
        std::vector<TaskMessageRecord> UnclaimedMessages;
        std::vector<DeleteTombstoneRecord> Tombstones;
        bool HasMoreActiveRuns{};
        bool HasMorePendingIntents{};
        bool HasMoreRecoverableTerminals{};
        bool HasMoreRecoverableGitWorktrees{};
        bool HasMoreRecoverableGitCheckpoints{};
        bool HasMoreUnclaimedMessages{};
        bool HasMoreTombstones{};
    };

    struct TaskRecoveryQueryResult
    {
        bool Succeeded{};
        TaskRecoverySnapshot Snapshot;
        std::string Error;
    };

    struct TaskStoreRuntimeActivitySnapshot
    {
        bool HasActiveRuns{};
        bool HasPendingIntents{};
        bool HasPendingMessages{};
        bool HasLiveTerminals{};
        bool HasGitWork{};

        [[nodiscard]] bool Active() const noexcept
        {
            return HasActiveRuns || HasPendingIntents ||
                   HasPendingMessages || HasLiveTerminals || HasGitWork;
        }
    };

    struct TaskStoreRuntimeActivityQueryResult
    {
        bool Succeeded{};
        TaskStoreRuntimeActivitySnapshot Snapshot;
        std::string Error;
    };

    enum class TaskStoreMaintenanceStatus
    {
        Succeeded,
        NotReady,
        InvalidRequest,
        StorageError,
    };

    struct TaskStoreMaintenanceResult
    {
        TaskStoreMaintenanceStatus Status{ TaskStoreMaintenanceStatus::StorageError };
        size_t RemovedCount{};
        bool HasMore{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == TaskStoreMaintenanceStatus::Succeeded;
        }
    };
}
