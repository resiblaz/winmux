// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskEnums.h"
#include "TaskIdentifiers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskCore
{
    struct AcceptanceCriterionRecord
    {
        uint32_t Position{};
        std::string Text;
        bool Satisfied{};
    };

    struct TaskRecord
    {
        TaskId Id;
        WorkspaceId Workspace;
        std::optional<TaskId> ForkedFromTaskId;
        std::string Title;
        std::string Description;
        std::vector<AcceptanceCriterionRecord> AcceptanceCriteria;
        TaskStatus Status{ TaskStatus::Todo };
        TaskPriority Priority{ TaskPriority::Medium };
        uint64_t Version{ 1 };
        bool ManualHold{};
        int64_t CreatedAtUnixMilliseconds{};
        int64_t UpdatedAtUnixMilliseconds{};
    };

    struct ExecutionContextRecord
    {
        ExecutionContextId Id;
        TaskId Task;
        uint64_t Revision{ 1 };
        ExecutionMode Mode{ ExecutionMode::Worktree };
        std::string SourceRoot;
        std::string RuntimeRoot;
        std::string RepositoryCommonDirectory;
        std::string WorktreePath;
        std::string Branch;
        std::string RelativeWorkingDirectory;
        std::string Provider;
        std::string Model;
        std::string Instructions;
        bool IdentityFrozen{};
        int64_t CreatedAtUnixMilliseconds{};
    };

    struct TaskTerminalRecord
    {
        TaskTerminalId Id;
        TaskId Task;
        TaskTerminalState State{ TaskTerminalState::NotCreated };
        uint64_t Generation{};
        std::optional<AgentSessionId> CurrentAgentSessionId;
        int64_t UpdatedAtUnixMilliseconds{};
    };

    struct AgentSessionRecord
    {
        AgentSessionId Id;
        TaskId Task;
        std::string Provider;
        std::string ProviderSessionId;
        AgentSessionState State{ AgentSessionState::Available };
        bool IsCurrent{};
        int64_t CreatedAtUnixMilliseconds{};
        int64_t UpdatedAtUnixMilliseconds{};
    };

    struct TaskRunRecord
    {
        TaskRunId Id;
        TaskId Task;
        ExecutionContextId ExecutionContext;
        std::optional<AgentSessionId> AgentSession;
        std::optional<TaskRunId> RetryOfRunId;
        TaskRunTrigger Trigger{ TaskRunTrigger::Initial };
        TaskRunState State{ TaskRunState::Queued };
        uint64_t Sequence{};
        int64_t CreatedAtUnixMilliseconds{};
        std::optional<int64_t> StartedAtUnixMilliseconds;
        std::optional<int64_t> FinishedAtUnixMilliseconds;
        std::string Result;
        std::string ReasonCode;
    };

    struct TaskMessageRecord
    {
        TaskMessageId Id;
        TaskId Task;
        TaskMessageState State{ TaskMessageState::Pending };
        uint64_t Revision{ 1 };
        std::string Content;
        std::optional<TaskRunId> ClaimedByRunId;
        int64_t CreatedAtUnixMilliseconds{};
    };

    struct TaskEventRecord
    {
        TaskEventId Id;
        TaskId Task;
        std::optional<TaskRunId> RunId;
        uint64_t AggregateSequence{};
        std::optional<uint64_t> RunSequence;
        std::string Type;
        std::string PayloadJson;
        int64_t CreatedAtUnixMilliseconds{};
    };

    struct CommandReceiptRecord
    {
        CommandId Id;
        TaskId Task;
        uint64_t ExpectedTaskVersion{};
        uint64_t CommittedTaskVersion{};
        std::string CommandName;
        std::string ResultJson;
        int64_t CreatedAtUnixMilliseconds{};
    };

    struct DeleteTombstoneRecord
    {
        TaskId Task;
        int64_t DeletedAtUnixMilliseconds{};
        int64_t ExpiresAtUnixMilliseconds{};
        std::string ReplayDigest;
    };
}
