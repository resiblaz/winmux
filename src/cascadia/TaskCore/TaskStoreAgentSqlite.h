// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "AgentProfile.h"
#include <winsqlite/winsqlite3.h>

namespace Microsoft::Terminal::TaskCore::details
{
    [[nodiscard]] bool MigrateAgentProfileSchema(sqlite3* database, std::string& error);
    [[nodiscard]] bool ValidateAgentProfileSchema(sqlite3* database, std::string& error);
    [[nodiscard]] bool ReadAgentProfile(sqlite3* database, const AgentProfileId& id, std::optional<AgentProfileRecord>& profile, std::string& error, bool includeDetails = true);
    [[nodiscard]] TaskStoreCommandStatus ResolveExecutionAgent(sqlite3* database, ExecutionContextRecord& context, std::string& error);
    [[nodiscard]] bool InsertExecutionContext(sqlite3* database, const ExecutionContextRecord& context, std::string& error);
}
