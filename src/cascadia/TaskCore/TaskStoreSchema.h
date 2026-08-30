// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreStatus.h"

#include <winsqlite/winsqlite3.h>

#include <cstdint>
#include <string>

namespace Microsoft::Terminal::TaskCore::details
{
    inline constexpr int32_t TaskDatabaseApplicationId{ 0x54534B31 }; // TSK1
    inline constexpr int32_t CurrentTaskDatabaseSchemaVersion{ 4 };

    [[nodiscard]] TaskStoreOpenStatus InitializeOrValidateTaskSchema(
        sqlite3* database,
        int32_t& observedVersion,
        std::string& error);
}
