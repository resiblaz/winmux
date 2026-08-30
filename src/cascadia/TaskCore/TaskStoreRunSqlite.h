// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRecords.h"

#include <winsqlite/winsqlite3.h>

#include <string>

namespace Microsoft::Terminal::TaskCore::details
{
    enum class TaskRunReferenceValidation
    {
        Valid,
        Invalid,
        StorageError,
    };

    [[nodiscard]] TaskRunReferenceValidation ValidateTaskRunReferences(
        sqlite3* database,
        const TaskRunRecord& run,
        std::string& error);

    [[nodiscard]] bool InsertTaskRun(
        sqlite3* database,
        const TaskRunRecord& run,
        uint64_t sequence,
        std::string& error);
}
