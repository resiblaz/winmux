// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRecords.h"

#include <winsqlite/winsqlite3.h>

#include <optional>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskCore::details
{
    struct StoredActiveTaskRun
    {
        TaskRunId Id;
        TaskRunState State{ TaskRunState::Queued };
    };

    [[nodiscard]] bool LoadActiveTaskRun(
        sqlite3* database,
        const TaskId& task,
        std::optional<StoredActiveTaskRun>& run,
        std::string& error);

    [[nodiscard]] bool LoadTaskMessage(
        sqlite3* database,
        const TaskMessageId& message,
        std::optional<TaskMessageRecord>& record,
        std::string& error);

    [[nodiscard]] bool CountUnclaimedTaskMessages(
        sqlite3* database,
        const TaskId& task,
        uint64_t& count,
        std::string& error);

    [[nodiscard]] bool InsertTaskMessage(
        sqlite3* database,
        const TaskMessageRecord& message,
        TaskMessageState state,
        const std::optional<TaskRunId>& claimedByRun,
        std::string& error);

    [[nodiscard]] bool NextRunMessagePosition(
        sqlite3* database,
        const TaskRunId& run,
        uint64_t& position,
        std::string& error);

    [[nodiscard]] bool InsertTaskRunMessage(
        sqlite3* database,
        const TaskRunId& run,
        const TaskMessageRecord& message,
        uint64_t position,
        std::string& error);

    [[nodiscard]] bool LoadClaimableTaskMessages(
        sqlite3* database,
        const TaskId& task,
        size_t maximumCount,
        std::vector<TaskMessageRecord>& messages,
        bool& exceedsMaximum,
        std::string& error);

    [[nodiscard]] bool ClaimTaskMessage(
        sqlite3* database,
        const TaskMessageRecord& message,
        const TaskRunId& run,
        int64_t timestamp,
        std::string& error);
}
