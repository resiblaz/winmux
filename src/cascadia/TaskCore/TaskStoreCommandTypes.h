// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRecords.h"
#include "TaskStatusPolicy.h"

#include <cstdint>
#include <string>

namespace Microsoft::Terminal::TaskCore
{
    inline constexpr size_t TaskStoreDigestCharacters{ 64 };
    inline constexpr size_t MaximumTaskStoreJsonBytes{ 64 * 1024 };
    inline constexpr size_t MaximumTaskRunResultBytes{ 256 * 1024 };
    inline constexpr size_t MaximumTaskBlockedReasonBytes{ 16 * 1024 };
    inline constexpr size_t MaximumTaskReasonCodeBytes{ 256 };
    inline constexpr size_t MaximumTaskMessageBytes{ 64 * 1024 };
    inline constexpr size_t MaximumUnclaimedTaskMessages{ 256 };
    inline constexpr size_t MaximumTaskMessagesPerRun{ 256 };
    inline constexpr size_t MaximumTaskProviderEventTypeBytes{ 128 };

    struct TaskCommandMetadata
    {
        CommandId Command;
        TaskEventId Event;
        uint64_t ExpectedTaskVersion{};
        std::string RequestDigest;
        std::string EventPayloadJson{ "{}" };
        std::string ReceiptResultJson{ "{}" };
        int64_t TimestampUnixMilliseconds{};
    };

    struct CreateTaskCommand
    {
        TaskCommandMetadata Metadata;
        TaskRecord Task;
        ExecutionContextRecord ExecutionContext;
        TaskTerminalRecord Terminal;
    };

    struct ChangeTaskStatusCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskStatus RequestedStatus{ TaskStatus::Todo };
        TaskStatusAuthority Authority{ TaskStatusAuthority::User };
    };

    struct CreateTaskRunCommand
    {
        TaskCommandMetadata Metadata;
        TaskRunRecord Run;
        bool ClearManualHold{};
    };

    struct RecordTaskAgentSessionCommand
    {
        TaskCommandMetadata Metadata;
        AgentSessionRecord Session;
    };

    struct AppendTaskProviderEventCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        std::optional<TaskRunId> Run;
        uint64_t ProviderSequence{};
        std::string EventType;
    };

    struct InitializeTaskGitWorktreeCommand
    {
        TaskCommandMetadata Metadata;
        TaskGitWorktreeRecord Worktree;
    };

    struct AdvanceTaskGitWorktreeCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        ExecutionContextId ExecutionContext;
        uint64_t ExpectedWorktreeRevision{};
        TaskGitWorktreeState RequestedState{ TaskGitWorktreeState::Planning };
        std::string SnapshotDigest;
        std::string BaselineCommit;
        bool HasUndeliveredWork{};
        std::string PreservedPath;
        std::string FailureReasonCode;
    };

    struct AdvanceTaskGitWorktreeFinalizationCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        ExecutionContextId ExecutionContext;
        uint64_t ExpectedWorktreeRevision{};
        TaskGitWorktreeState RequestedState{ TaskGitWorktreeState::Finalizing };
        bool HasUndeliveredWork{};
        bool CleanupEligible{};
        std::string PreservedPath;
        std::string FailureReasonCode;
    };

    struct BeginTaskRunGitCheckpointCommand
    {
        TaskCommandMetadata Metadata;
        TaskRunGitCheckpointRecord Checkpoint;
    };

    struct AdvanceTaskRunGitCheckpointCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskRunId Run;
        GitCheckpointId Checkpoint;
        uint64_t ExpectedCheckpointRevision{};
        uint64_t ExpectedWorktreeRevision{};
        TaskGitCheckpointState RequestedState{ TaskGitCheckpointState::Pending };
        std::string EndCommit;
        std::string CheckpointCommit;
        bool AutoCheckpointed{};
        std::string ChangedPathsJson{ "[]" };
        std::string DiffSummaryJson{ "{}" };
        bool HasUndeliveredWork{};
        std::string PreservedPath;
        std::string ReasonCode;
    };

    struct ChangeTaskRunStateCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskRunId Run;
        TaskRunState RequestedState{ TaskRunState::Queued };
        std::string Result;
        std::string ReasonCode;
        std::optional<AgentSessionId> AgentSession;
    };

    enum class TaskAgentMutationKind : uint8_t
    {
        StatusInProgress,
        StatusInReview,
        StatusBlocked,
        Result,
        Count,
    };

    struct ApplyTaskAgentMutationCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskRunId Run;
        TaskAgentMutationKind Kind{ TaskAgentMutationKind::StatusInProgress };
        std::string Text;
    };

    struct ChangeTaskTerminalStateCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskTerminalState RequestedState{ TaskTerminalState::NotCreated };
        uint64_t RequestedGeneration{};
        bool ExplicitOpen{};
        bool ClearManualHold{};
    };

    struct StartTaskIntentCommand
    {
        TaskCommandMetadata Metadata;
        TaskIntentRecord Intent;
    };

    struct AdvanceTaskIntentCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskIntentId Intent;
        uint64_t ExpectedIntentRevision{};
        TaskIntentStage RequestedStage{ TaskIntentStage::Requested };
        std::optional<int64_t> DeadlineAtUnixMilliseconds;
        std::string DetailJson{ "{}" };
        std::string ReasonCode;
    };

    struct ChangeTaskManualHoldCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        bool RequestedHold{};
    };

    struct DeleteTaskCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskIntentId Intent;
        uint64_t ExpectedIntentRevision{};
    };

    struct EnqueueTaskMessageCommand
    {
        TaskCommandMetadata Metadata;
        TaskMessageRecord Message;
    };

    struct ReviseTaskMessageCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskMessageId Message;
        uint64_t ExpectedMessageRevision{};
        std::optional<std::string> ReplacementContent;
        bool Remove{};
    };

    struct ClaimTaskMessagesAndCreateRunCommand
    {
        TaskCommandMetadata Metadata;
        TaskRunRecord Run;
    };
}
