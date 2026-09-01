// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolRpcJson.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr size_t MaximumTaskProjectionItems{ 512 };
    inline constexpr size_t MaximumTaskProjectionRuns{ 256 };
    inline constexpr size_t MaximumTaskProjectionMessages{ 256 };
    inline constexpr size_t MaximumTaskProjectionEvents{ 1000 };
    inline constexpr size_t MaximumTaskProjectionAcceptance{ 256 };

    enum class TaskProjectionStatus : uint8_t
    {
        Backlog,
        Todo,
        InProgress,
        InReview,
        Blocked,
        Done,
        Cancelled,
    };

    enum class TaskProjectionPriority : uint8_t
    {
        Low,
        Medium,
        High,
    };

    enum class TaskProjectionExecutionMode : uint8_t
    {
        Direct,
        Worktree,
    };

    enum class TaskProjectionTerminalState : uint8_t
    {
        NotCreated,
        Starting,
        Ready,
        Busy,
        Exited,
        Suspended,
        Error,
        Closed,
    };

    enum class TaskProjectionRunState : uint8_t
    {
        Queued,
        Preparing,
        WaitingForDirectory,
        Starting,
        Running,
        Completed,
        Failed,
        Cancelled,
        Interrupted,
    };

    struct TaskProjectionAcceptanceRecord
    {
        std::string Text;
        bool Satisfied{};
    };

    struct TaskProjectionExecutionRecord
    {
        std::string Id;
        uint64_t Revision{};
        TaskProjectionExecutionMode Mode{ TaskProjectionExecutionMode::Direct };
        std::string SourceRoot;
        std::string RuntimeRoot;
        std::string WorktreePath;
        std::string Branch;
        std::string RelativeWorkingDirectory;
        std::string Provider;
        std::string Model;
        std::string Instructions;
    };

    struct TaskProjectionTerminalRecord
    {
        TaskProjectionTerminalState State{ TaskProjectionTerminalState::NotCreated };
        uint64_t Generation{};
        std::optional<std::string> CurrentAgentSessionId;
    };

    struct TaskProjectionAgentSessionRecord
    {
        std::string Id;
        std::string Provider;
        std::string ProviderSessionId;
        uint8_t State{};
        bool IsCurrent{};
    };

    struct TaskProjectionRunRecord
    {
        std::string Id;
        std::string ExecutionContextId;
        std::optional<std::string> AgentSessionId;
        std::optional<std::string> RetryOfRunId;
        uint8_t Trigger{};
        TaskProjectionRunState State{ TaskProjectionRunState::Queued };
        uint64_t Sequence{};
        int64_t CreatedAtUnixMilliseconds{};
        std::optional<int64_t> StartedAtUnixMilliseconds;
        std::optional<int64_t> FinishedAtUnixMilliseconds;
        std::string Result;
        std::string ReasonCode;
    };

    struct TaskProjectionMessageRecord
    {
        std::string Id;
        uint8_t Author{};
        uint8_t State{};
        uint64_t Revision{};
        std::string Content;
        std::optional<std::string> ClaimedByRunId;
        int64_t CreatedAtUnixMilliseconds{};
        int64_t UpdatedAtUnixMilliseconds{};
    };

    struct TaskProjectionEventRecord
    {
        std::string Id;
        std::optional<std::string> RunId;
        uint64_t AggregateSequence{};
        std::optional<uint64_t> RunSequence;
        std::string Type;
        std::string PayloadJson{ "{}" };
        int64_t CreatedAtUnixMilliseconds{};
    };

    struct TaskProjectionRecord
    {
        std::string Id;
        std::string WorkspaceId;
        std::string Title;
        std::string Description;
        std::vector<TaskProjectionAcceptanceRecord> Acceptance;
        TaskProjectionStatus Status{ TaskProjectionStatus::Todo };
        TaskProjectionPriority Priority{ TaskProjectionPriority::Medium };
        uint64_t Version{};
        bool ManualHold{};
        int64_t CreatedAtUnixMilliseconds{};
        int64_t UpdatedAtUnixMilliseconds{};
        TaskProjectionExecutionRecord Execution;
        TaskProjectionTerminalRecord Terminal;
        std::vector<TaskProjectionAgentSessionRecord> AgentSessions;
        std::vector<TaskProjectionRunRecord> Runs;
        std::vector<TaskProjectionMessageRecord> Messages;
        std::vector<TaskProjectionEventRecord> Events;
        uint64_t LastEventSequence{};
    };

    struct TaskProjectionQueryRequest
    {
        std::optional<std::string> TaskId;
        std::optional<std::string> WorkspaceId;
        bool IncludeCompleted{};
        size_t EventLimit{ 200 };
    };

    struct TaskProjectionQueryResult
    {
        uint64_t Generation{};
        std::vector<TaskProjectionRecord> Tasks;
    };

    enum class TaskUiCommandOperation : uint8_t
    {
        Create,
        Start,
        Stop,
        Close,
        MarkDone,
        Reopen,
        Count,
    };

    struct TaskUiCreateDraft
    {
        std::string WorkspaceId;
        std::string Title;
        std::string Description;
        std::vector<std::string> Acceptance;
        TaskProjectionPriority Priority{ TaskProjectionPriority::Medium };
        TaskProjectionExecutionMode ExecutionMode{ TaskProjectionExecutionMode::Worktree };
        std::string SourceRoot;
        std::string RelativeWorkingDirectory;
        std::string Provider{ "codex" };
        std::string Model;
        std::string Instructions;
    };

    struct TaskUiCommandRequest
    {
        std::string CommandId;
        TaskUiCommandOperation Operation{ TaskUiCommandOperation::Create };
        std::string TaskId;
        uint64_t ExpectedTaskVersion{};
        std::optional<TaskUiCreateDraft> Draft;
    };

    struct TaskUiCommandResult
    {
        std::string CommandId;
        std::string TaskId;
        std::optional<std::string> RunId;
        uint64_t TaskVersion{};
        uint64_t EventSequence{};
        bool Replayed{};
        bool Pending{};
    };

    [[nodiscard]] bool ValidateTaskProjectionQuery(const TaskProjectionQueryRequest& value, std::string& error) noexcept;
    [[nodiscard]] bool ValidateTaskProjection(const TaskProjectionRecord& value, std::string& error) noexcept;
    [[nodiscard]] bool ValidateTaskUiCommand(const TaskUiCommandRequest& value, std::string& error) noexcept;
    [[nodiscard]] std::string_view TaskUiCommandOperationName(TaskUiCommandOperation value) noexcept;
    [[nodiscard]] std::optional<TaskUiCommandOperation> TaskUiCommandOperationFromName(std::string_view value) noexcept;

    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskProjectionQueryRequest> DecodeTaskProjectionQueryRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskProjectionQueryRequest(const TaskProjectionQueryRequest& value);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskProjectionQueryResult> DecodeTaskProjectionQueryResult(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskProjectionQueryResult(const TaskProjectionQueryResult& value);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskUiCommandRequest> DecodeTaskUiCommandRequest(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskUiCommandRequest(const TaskUiCommandRequest& value);
    [[nodiscard]] ProtocolRpcJsonDecodeResult<TaskUiCommandResult> DecodeTaskUiCommandResult(std::string_view payload);
    [[nodiscard]] ProtocolRpcJsonEncodeResult EncodeTaskUiCommandResult(const TaskUiCommandResult& value);
}
