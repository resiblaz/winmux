// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreCommandTypes.h"
#include "TaskStoreResults.h"

#include <winsqlite/winsqlite3.h>

#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskCore::details
{
    struct StoredReceipt
    {
        TaskId Task;
        std::string CommandName;
        std::string RequestDigest;
        uint64_t ExpectedVersion{};
        uint64_t CommittedVersion{};
        uint64_t LastEventSequence{};
        std::string ResultJson;
    };

    struct StoredTaskState
    {
        uint64_t Version{};
        TaskStatus Status{ TaskStatus::Todo };
        bool ManualHold{};
        int64_t CreatedAtUnixMilliseconds{};
        int64_t UpdatedAtUnixMilliseconds{};
    };

    struct StoredTaskRunState
    {
        TaskId Task;
        TaskRunState State{ TaskRunState::Queued };
        int64_t CreatedAtUnixMilliseconds{};
        std::optional<int64_t> StartedAtUnixMilliseconds;
        std::optional<int64_t> FinishedAtUnixMilliseconds;
        std::string Result;
        std::string ReasonCode;
        std::optional<AgentSessionId> AgentSession;
    };

    struct StoredTaskTerminalState
    {
        TaskId Task;
        TaskTerminalState State{ TaskTerminalState::NotCreated };
        uint64_t Generation{};
        int64_t UpdatedAtUnixMilliseconds{};
    };

    [[nodiscard]] TaskStoreCommandResult CommandFailure(
        TaskStoreCommandStatus status,
        std::string error,
        TransitionError transition = TransitionError::None);

    [[nodiscard]] bool LookupReceipt(
        sqlite3* database,
        const CommandId& command,
        std::optional<StoredReceipt>& receipt,
        std::string& error);

    [[nodiscard]] std::optional<TaskStoreCommandResult> EvaluateReceiptReplay(
        const std::optional<StoredReceipt>& receipt,
        const TaskCommandMetadata& metadata,
        const TaskId& task,
        std::string_view commandName);

    [[nodiscard]] bool LoadTaskState(
        sqlite3* database,
        const TaskId& task,
        std::optional<StoredTaskState>& state,
        std::string& error);

    [[nodiscard]] bool IsTaskTombstoned(
        sqlite3* database,
        const TaskId& task,
        bool& tombstoned,
        std::string& error);

    [[nodiscard]] TaskStoreCommandResult MissingTaskFailure(
        sqlite3* database,
        const TaskId& task,
        std::string& error);

    [[nodiscard]] bool LoadTaskRunState(
        sqlite3* database,
        const TaskRunId& run,
        std::optional<StoredTaskRunState>& state,
        std::string& error);

    [[nodiscard]] bool LoadTaskTerminalState(
        sqlite3* database,
        const TaskId& task,
        std::optional<StoredTaskTerminalState>& state,
        std::string& error);

    [[nodiscard]] bool LoadTaskIntent(
        sqlite3* database,
        const TaskId& task,
        std::optional<TaskIntentRecord>& intent,
        std::string& error);

    [[nodiscard]] bool RowBelongsToTask(
        sqlite3* database,
        const char* sql,
        const TaskId& task,
        std::string_view rowId,
        bool& value,
        std::string& error);

    [[nodiscard]] bool HasActiveRun(sqlite3* database, const TaskId& task, bool& value, std::string& error);
    [[nodiscard]] bool HasUnclaimedMessages(sqlite3* database, const TaskId& task, bool& value, std::string& error);
    [[nodiscard]] bool HasPendingAutomaticRetry(sqlite3* database, const TaskId& task, bool& value, std::string& error);

    [[nodiscard]] bool NextAggregateSequence(sqlite3* database, const TaskId& task, uint64_t& sequence, std::string& error);
    [[nodiscard]] bool NextRunSequence(sqlite3* database, const TaskId& task, uint64_t& sequence, std::string& error);
    [[nodiscard]] bool NextRunEventSequence(sqlite3* database, const TaskRunId& run, uint64_t& sequence, std::string& error);

    [[nodiscard]] bool UpdateTaskVersion(
        sqlite3* database,
        const TaskId& task,
        uint64_t expectedVersion,
        uint64_t committedVersion,
        int64_t timestamp,
        std::string& error);

    [[nodiscard]] bool AppendEvent(
        sqlite3* database,
        const TaskCommandMetadata& metadata,
        const TaskId& task,
        const std::optional<TaskRunId>& run,
        uint64_t aggregateSequence,
        const std::optional<uint64_t>& runSequence,
        std::string_view type,
        std::string& error);

    [[nodiscard]] bool InsertReceipt(
        sqlite3* database,
        const TaskCommandMetadata& metadata,
        const TaskId& task,
        std::string_view commandName,
        uint64_t committedVersion,
        uint64_t lastEventSequence,
        std::string& error);
}
