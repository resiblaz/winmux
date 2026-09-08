// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "SqliteTaskStoreInternal.h"
#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore
{
    TaskSessionIndexResult SqliteTaskStore::QueryTaskSessionIndex(uint64_t afterSequence, size_t limit) const
    {
        TaskSessionIndexResult result; result.AfterSequence = afterSequence;
        if (!IsReady()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = LastError(); return result; }
        if (afterSequence > INT64_MAX || !limit || limit > 100)
        { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = "Invalid task session cursor"; return result; }
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        details::SqliteStatement page{ _impl->Database, "SELECT sequence,provider_session_id FROM task_owned_session WHERE sequence>?1 ORDER BY sequence LIMIT ?2;", result.Error };
        if (!page || !details::BindInt64(_impl->Database, page.Get(), 1, static_cast<int64_t>(afterSequence), result.Error) ||
            !details::BindInt64(_impl->Database, page.Get(), 2, static_cast<int64_t>(limit + 1), result.Error)) return result;
        int step;
        while ((step = sqlite3_step(page.Get())) == SQLITE_ROW)
        {
            if (result.Sessions.size() == limit) { result.HasMore = true; break; }
            result.AfterSequence = static_cast<uint64_t>(sqlite3_column_int64(page.Get(), 0));
            result.Sessions.push_back({ result.AfterSequence, details::ColumnText(page.Get(), 1) });
        }
        if (step != SQLITE_DONE && step != SQLITE_ROW) { result.Error = details::SqliteError(_impl->Database, step, "read Task session index"); return result; }
        result.Status = TaskStoreCommandStatus::Succeeded; return result;
    }
}
