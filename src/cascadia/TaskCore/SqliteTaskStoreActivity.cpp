// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore
{
    TaskStoreRuntimeActivityQueryResult SqliteTaskStore::LoadRuntimeActivity() const
    {
        TaskStoreRuntimeActivityQueryResult result;
        if (!IsReady())
        {
            result.Error = LastError();
            return result;
        }

        std::scoped_lock lock{ _impl->DatabaseMutex };
        std::string error;
        constexpr auto sql = R"SQL(
SELECT
    EXISTS(SELECT 1 FROM task_run WHERE state IN (0,1,2,3,4)),
    EXISTS(SELECT 1 FROM task_intent WHERE stage NOT IN (7,8)),
    EXISTS(SELECT 1 FROM task_message WHERE state IN (0,1)),
    EXISTS(SELECT 1 FROM task_terminal WHERE state IN (1,2,3)),
    EXISTS(SELECT 1 FROM task_git_worktree WHERE state IN (0,1,2,3,4,6));
)SQL";
        details::SqliteStatement statement{ _impl->Database, sql, error };
        if (!statement)
        {
            result.Error = std::move(error);
            return result;
        }
        const auto step = sqlite3_step(statement.Get());
        if (step != SQLITE_ROW)
        {
            result.Error = details::SqliteError(
                _impl->Database,
                step,
                "load Runtime activity");
            return result;
        }

        result.Snapshot.HasActiveRuns = sqlite3_column_int(statement.Get(), 0) != 0;
        result.Snapshot.HasPendingIntents = sqlite3_column_int(statement.Get(), 1) != 0;
        result.Snapshot.HasPendingMessages = sqlite3_column_int(statement.Get(), 2) != 0;
        result.Snapshot.HasLiveTerminals = sqlite3_column_int(statement.Get(), 3) != 0;
        result.Snapshot.HasGitWork = sqlite3_column_int(statement.Get(), 4) != 0;
        if (sqlite3_step(statement.Get()) != SQLITE_DONE)
        {
            result.Error = "Runtime activity query returned unexpected rows";
            return result;
        }
        result.Succeeded = true;
        return result;
    }
}
