// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "TaskProjectionRpc.h"
#include <array>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr size_t MaximumTaskSummaryItems{ 100 };
    enum class TaskSummaryOrder : uint8_t { Updated, BoardOrder };
    enum class TaskBoardPlacement : uint8_t { Before, After, End };
    enum class TaskBoardOperation : uint8_t { Reorder, MovePlanning, Start, Continue, StopThenMove, SubmitForReview, MarkDone, ReopenOnly, UpdateAcceptance, Undo };
    struct TaskSummaryQueryRequest
    {
        std::optional<std::string> ProjectId;
        std::optional<TaskProjectionStatus> Status;
        std::optional<TaskProjectionStatus> Bucket;
        bool OpenOnly{};
        std::string Search;
        TaskSummaryOrder Sort{ TaskSummaryOrder::Updated };
        std::string AfterId;
        int64_t AfterValue{};
        uint64_t ExpectedColumnRevision{};
        size_t Limit{ 50 };
    };
    struct TaskSummaryProjection
    {
        std::string Id;
        std::string ProjectId;
        std::string ProjectName;
        std::string Title;
        TaskProjectionStatus Status{ TaskProjectionStatus::Todo };
        TaskProjectionPriority Priority{ TaskProjectionPriority::Medium };
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
        TaskProjectionExecutionMode Mode{ TaskProjectionExecutionMode::Direct };
        std::string RunId;
        std::optional<TaskProjectionRunState> RunState;
        uint64_t RunSequence{};
        uint64_t LastEventSequence{};
        bool HasPendingMessages{};
        bool HasPendingIntent{};
        bool ArchivedProject{};
    };
    struct TaskSummaryQueryResult
    {
        std::vector<TaskSummaryProjection> Tasks;
        std::array<uint64_t, 7> Counts{};
        uint64_t Total{};
        uint64_t Generation{};
        uint64_t ColumnRevision{};
        std::string NextAfterId;
        int64_t NextAfterValue{};
    };
    struct TaskBoardCommandRequest
    {
        std::string CommandId;
        std::string TaskId;
        std::string ProjectId;
        uint64_t ExpectedTaskVersion{};
        TaskBoardOperation Operation{ TaskBoardOperation::MovePlanning };
        TaskProjectionStatus Target{ TaskProjectionStatus::Todo };
        uint64_t ExpectedColumnRevision{};
        std::optional<std::string> AnchorId;
        TaskBoardPlacement Placement{ TaskBoardPlacement::End };
        std::optional<std::string> RelatedRunId;
        std::string Text;
        std::vector<std::pair<uint32_t, bool>> Acceptance;
        std::optional<std::string> UndoOfCommandId;
    };
    struct TaskBoardCommandResult
    {
        std::string CommandId;
        std::string TaskId;
        uint64_t TaskVersion{};
        uint64_t ColumnRevision{};
        uint64_t EventSequence{};
        std::optional<std::string> RunId;
        std::optional<std::string> OperationId;
        bool Pending{};
        bool Replayed{};
    };
    struct TaskBoardOperationQueryRequest { std::string OperationId; };
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskSummaryQueryRequest> DecodeTaskSummaryQueryRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskSummaryQueryResult> DecodeTaskSummaryQueryResult(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskBoardCommandRequest> DecodeTaskBoardCommandRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskBoardCommandResult> DecodeTaskBoardCommandResult(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskBoardOperationQueryRequest> DecodeTaskBoardOperationQueryRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskSummaryQueryRequest(const TaskSummaryQueryRequest& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskSummaryQueryResult(const TaskSummaryQueryResult& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskBoardCommandRequest(const TaskBoardCommandRequest& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskBoardCommandResult(const TaskBoardCommandResult& value);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskBoardOperationQueryRequest(const TaskBoardOperationQueryRequest& value);
}
