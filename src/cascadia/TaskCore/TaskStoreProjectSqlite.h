// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "ProjectCatalog.h"
#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    [[nodiscard]] bool MigrateProjectSchema(sqlite3* database, std::string& error);
    [[nodiscard]] bool ValidateProjectSchema(sqlite3* database, std::string& error);
    [[nodiscard]] bool ReadProject(sqlite3* database, const WorkspaceId& id, std::optional<ProjectRecord>& result, std::string& error);
    [[nodiscard]] bool ProjectHasWork(sqlite3* database, const WorkspaceId& id, bool& result, std::string& error);
}
