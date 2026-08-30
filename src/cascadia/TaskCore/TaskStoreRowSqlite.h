// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRecords.h"

#include <winsqlite/winsqlite3.h>

namespace Microsoft::Terminal::TaskCore::details
{
    [[nodiscard]] TaskRecord ReadTaskRow(sqlite3_stmt* statement);
    [[nodiscard]] AcceptanceCriterionRecord ReadAcceptanceCriterionRow(sqlite3_stmt* statement, int firstColumn = 0);
    [[nodiscard]] ExecutionContextRecord ReadExecutionContextRow(sqlite3_stmt* statement);
    [[nodiscard]] TaskTerminalRecord ReadTaskTerminalRow(sqlite3_stmt* statement);
    [[nodiscard]] AgentSessionRecord ReadAgentSessionRow(sqlite3_stmt* statement);
    [[nodiscard]] TaskRunRecord ReadTaskRunRow(sqlite3_stmt* statement);
    [[nodiscard]] TaskMessageRecord ReadTaskMessageRow(sqlite3_stmt* statement);
    [[nodiscard]] TaskRunMessageRecord ReadTaskRunMessageRow(sqlite3_stmt* statement);
    [[nodiscard]] TaskEventRecord ReadTaskEventRow(sqlite3_stmt* statement);
    [[nodiscard]] TaskIntentRecord ReadTaskIntentRow(sqlite3_stmt* statement);
    [[nodiscard]] DeleteTombstoneRecord ReadDeleteTombstoneRow(sqlite3_stmt* statement);
}
