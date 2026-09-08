// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "TaskStoreResults.h"
#include "TaskStoreCommandTypes.h"
#include <array>

namespace Microsoft::Terminal::TaskCore
{
    inline constexpr size_t MaximumTaskSummaryPageSize{ 100 };
    constexpr TaskStatus TaskBoardBucket(TaskStatus status) noexcept { return status == TaskStatus::Blocked ? TaskStatus::InProgress : status; }
    enum class TaskSummarySort : uint8_t { Updated, BoardOrder };
    enum class BoardPlacement : uint8_t { Before, After, End };
    enum class TaskBoardEffect : uint8_t { Reorder, MovePlanning, Start, Continue, StopThenMove, SubmitForReview, MarkDone, ReopenOnly, UpdateAcceptance, Undo };

    struct TaskSummaryQuery
    {
        std::optional<WorkspaceId> Workspace;
        std::optional<TaskStatus> Status;
        std::optional<TaskStatus> Bucket;
        bool OpenOnly{};
        std::string Search;
        TaskSummarySort Sort{ TaskSummarySort::Updated };
        std::string AfterId;
        int64_t AfterValue{};
        uint64_t ExpectedColumnRevision{};
        size_t Limit{ 50 };
    };

    struct TaskSummaryRecord
    {
        TaskId Id;
        WorkspaceId Workspace;
        std::string ProjectName;
        std::string Title;
        TaskStatus Status{ TaskStatus::Todo };
        TaskPriority Priority{ TaskPriority::Medium };
        uint64_t Version{};
        bool ManualHold{};
        int64_t UpdatedAtUnixMilliseconds{};
        int64_t Rank{};
        uint64_t ColumnRevision{};
        std::string ExecutionContextId;
        std::string AgentId;
        uint64_t AgentRevision{};
        std::string AgentName;
        std::string Model;
        ExecutionMode Mode{ ExecutionMode::Direct };
        std::string RunId;
        std::optional<TaskRunState> RunState;
        uint64_t RunSequence{};
        uint64_t LastEventSequence{};
        bool HasPendingMessages{};
        bool HasPendingIntent{};
        bool ArchivedProject{};
    };

    struct TaskSummaryQueryResult
    {
        TaskStoreCommandStatus Status{ TaskStoreCommandStatus::StorageError };
        std::vector<TaskSummaryRecord> Tasks;
        std::array<uint64_t, 7> Counts{};
        uint64_t Total{};
        uint64_t Generation{};
        uint64_t ColumnRevision{};
        std::string NextAfterId;
        int64_t NextAfterValue{};
        std::string Error;
        bool Succeeded() const noexcept { return Status == TaskStoreCommandStatus::Succeeded; }
    };

    struct TaskBoardCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        WorkspaceId Workspace;
        TaskBoardEffect Effect{ TaskBoardEffect::MovePlanning };
        TaskStatus Target{ TaskStatus::Todo };
        uint64_t ExpectedColumnRevision{};
        std::optional<TaskId> Anchor;
        BoardPlacement Placement{ BoardPlacement::End };
        std::optional<TaskRunId> RelatedRun;
        std::string Text;
        TaskRunId NewRun;
        TaskMessageId NewMessage;
        TaskIntentId Intent;
        std::vector<std::pair<uint32_t, bool>> Acceptance;
        std::optional<CommandId> UndoOf;
    };

    struct TaskBoardCommandResult
    {
        TaskStoreCommandStatus Status{ TaskStoreCommandStatus::StorageError };
        TaskId Task;
        uint64_t TaskVersion{};
        uint64_t ColumnRevision{};
        uint64_t EventSequence{};
        std::optional<TaskRunId> Run;
        std::optional<CommandId> Operation;
        bool Pending{};
        std::string Error;
        bool Succeeded() const noexcept { return Status == TaskStoreCommandStatus::Succeeded || Status == TaskStoreCommandStatus::Replayed; }
    };
}
