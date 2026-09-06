// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStoreInternal.h"
#include "TaskStoreAgentSqlite.h"
#include "TaskStoreCommandSqlite.h"
#include "TaskStoreCommandValidation.h"
#include "TaskStoreRowSqlite.h"
#include "TaskStoreSqlite.h"
#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    TaskStoreCommandResult SqliteTaskStore::UpdateTaskExecutionAgent(const UpdateTaskExecutionAgentCommand& command)
    {
        constexpr std::string_view commandName{ "UpdateTaskExecutionAgent" };
        std::string error;
        if (!IsReady()) return details::CommandFailure(TaskStoreCommandStatus::NotReady, LastError());
        if (!details::ValidateCommandMetadata(command.Metadata, error) || !IsCanonicalUuid(command.Task.Value()) ||
            !IsCanonicalUuid(command.Context.Value()))
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Execution Agent command is invalid");
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        auto db = _impl->Database;
        details::SqliteTransaction transaction{ db, error };
        if (!transaction) return details::CommandFailure(TaskStoreCommandStatus::StorageError, error);
        std::optional<details::StoredReceipt> receipt;
        if (!details::LookupReceipt(db, command.Metadata.Command, receipt, error))
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, error);
        if (const auto replay = details::EvaluateReceiptReplay(receipt, command.Metadata, command.Task, commandName)) return *replay;
        std::optional<details::StoredTaskState> task;
        std::optional<TaskIntentRecord> intent;
        bool active{};
        if (!details::LoadTaskState(db, command.Task, task, error) ||
            !details::LoadTaskIntent(db, command.Task, intent, error) || !details::HasActiveRun(db, command.Task, active, error))
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, error);
        if (!task) return details::MissingTaskFailure(db, command.Task, error);
        if (task->Version != command.Metadata.ExpectedTaskVersion)
            return details::CommandFailure(TaskStoreCommandStatus::VersionConflict, "Task changed; reload its Agent configuration");
        if (active) return details::CommandFailure(TaskStoreCommandStatus::ActiveRunExists, "Finish or stop the active Run before changing Agent configuration");
        if (intent && intent->Stage != TaskIntentStage::Completed && intent->Stage != TaskIntentStage::Failed)
            return details::CommandFailure(TaskStoreCommandStatus::ActiveIntentExists, "Task lifecycle operation is still pending");
        if (task->Version >= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) || command.Metadata.TimestampUnixMilliseconds < task->UpdatedAtUnixMilliseconds)
            return details::CommandFailure(TaskStoreCommandStatus::InvalidCommand, "Task version or timestamp is invalid");
        ExecutionContextRecord context;
        {
            details::SqliteStatement statement{ db, R"SQL(
SELECT id,task_id,revision,mode,source_root,runtime_root,repository_common_directory,worktree_path,branch,
 relative_working_directory,provider,model,instructions,identity_frozen,created_at,source_agent_id,source_agent_revision,agent_name_snapshot
FROM execution_context WHERE task_id=?1 ORDER BY revision DESC LIMIT 1;
)SQL", error };
            if (!statement || !details::BindText(db, statement.Get(), 1, command.Task.Value(), error))
                return details::CommandFailure(TaskStoreCommandStatus::StorageError, error);
            const auto step = sqlite3_step(statement.Get());
            if (step != SQLITE_ROW) return details::CommandFailure(TaskStoreCommandStatus::StorageError, "Task execution context is missing");
            context = details::ReadExecutionContextRow(statement.Get());
        }
        if (context.Revision >= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
            return details::CommandFailure(TaskStoreCommandStatus::CapacityExceeded, "Execution revision capacity was reached");
        context.Id = command.Context;
        ++context.Revision;
        context.CreatedAtUnixMilliseconds = command.Metadata.TimestampUnixMilliseconds;
        context.SourceAgentId = command.Agent ? std::optional{ command.Agent->Id } : std::nullopt;
        context.SourceAgentRevision = command.Agent ? command.Agent->Revision : 0;
        context.AgentNameSnapshot.clear();
        context.Provider = command.Provider;
        context.Model = command.Model;
        context.Instructions = command.Instructions;
        if (command.Agent)
        {
            const auto resolved = details::ResolveExecutionAgent(db, context, error);
            if (resolved != TaskStoreCommandStatus::Succeeded) return details::CommandFailure(resolved, error);
        }
        else
        {
            AgentProfileContent content{ "Task", {}, context.Provider, context.Model, context.Instructions };
            if (!NormalizeAgentProfileContent(content, error)) return details::CommandFailure(TaskStoreCommandStatus::InvalidRecord, error);
        }
        uint64_t sequence{};
        const auto version = task->Version + 1;
        if (!details::InsertExecutionContext(db, context, error) || !details::NextAggregateSequence(db, command.Task, sequence, error) ||
            !details::UpdateTaskVersion(db, command.Task, task->Version, version, command.Metadata.TimestampUnixMilliseconds, error) ||
            !details::AppendEvent(db, command.Metadata, command.Task, std::nullopt, sequence, std::nullopt, "ExecutionAgentChanged", error) ||
            !details::InsertReceipt(db, command.Metadata, command.Task, commandName, version, sequence, error) || !transaction.Commit(error))
            return details::CommandFailure(TaskStoreCommandStatus::StorageError, error);
        TaskStoreCommandResult result;
        result.Status = TaskStoreCommandStatus::Succeeded;
        result.CommittedTaskVersion = version;
        result.LastEventSequence = sequence;
        result.ResultJson = command.Metadata.ReceiptResultJson;
        return result;
    }
}
