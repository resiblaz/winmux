// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreRowSqlite.h"

#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    namespace
    {
        template<typename TId>
        std::optional<TId> _optionalId(sqlite3_stmt* statement, const int column)
        {
            const auto value = ColumnOptionalText(statement, column);
            return value ? std::optional<TId>{ TId{ value.value() } } : std::nullopt;
        }
    }

    TaskRecord ReadTaskRow(sqlite3_stmt* statement)
    {
        TaskRecord task;
        task.Id = TaskId{ ColumnText(statement, 0) };
        task.Workspace = WorkspaceId{ ColumnText(statement, 1) };
        task.ForkedFromTaskId = _optionalId<TaskId>(statement, 2);
        task.Title = ColumnText(statement, 3);
        task.Description = ColumnText(statement, 4);
        task.Status = static_cast<TaskStatus>(sqlite3_column_int(statement, 5));
        task.Priority = static_cast<TaskPriority>(sqlite3_column_int(statement, 6));
        task.Version = static_cast<uint64_t>(sqlite3_column_int64(statement, 7));
        task.ManualHold = sqlite3_column_int(statement, 8) != 0;
        task.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement, 9);
        task.UpdatedAtUnixMilliseconds = sqlite3_column_int64(statement, 10);
        return task;
    }

    AcceptanceCriterionRecord ReadAcceptanceCriterionRow(sqlite3_stmt* statement, const int firstColumn)
    {
        return AcceptanceCriterionRecord{
            static_cast<uint32_t>(sqlite3_column_int64(statement, firstColumn)),
            ColumnText(statement, firstColumn + 1),
            sqlite3_column_int(statement, firstColumn + 2) != 0,
        };
    }

    ExecutionContextRecord ReadExecutionContextRow(sqlite3_stmt* statement)
    {
        ExecutionContextRecord context;
        context.Id = ExecutionContextId{ ColumnText(statement, 0) };
        context.Task = TaskId{ ColumnText(statement, 1) };
        context.Revision = static_cast<uint64_t>(sqlite3_column_int64(statement, 2));
        context.Mode = static_cast<ExecutionMode>(sqlite3_column_int(statement, 3));
        context.SourceRoot = ColumnText(statement, 4);
        context.RuntimeRoot = ColumnText(statement, 5);
        context.RepositoryCommonDirectory = ColumnText(statement, 6);
        context.WorktreePath = ColumnText(statement, 7);
        context.Branch = ColumnText(statement, 8);
        context.RelativeWorkingDirectory = ColumnText(statement, 9);
        context.Provider = ColumnText(statement, 10);
        context.Model = ColumnText(statement, 11);
        context.Instructions = ColumnText(statement, 12);
        context.IdentityFrozen = sqlite3_column_int(statement, 13) != 0;
        context.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement, 14);
        return context;
    }

    TaskGitWorktreeRecord ReadTaskGitWorktreeRow(sqlite3_stmt* statement)
    {
        TaskGitWorktreeRecord worktree;
        worktree.Task = TaskId{ ColumnText(statement, 0) };
        worktree.ExecutionContext = ExecutionContextId{ ColumnText(statement, 1) };
        worktree.Revision = static_cast<uint64_t>(sqlite3_column_int64(statement, 2));
        worktree.State = static_cast<TaskGitWorktreeState>(sqlite3_column_int(statement, 3));
        worktree.RepositoryRoot = ColumnText(statement, 4);
        worktree.RepositoryCommonDirectory = ColumnText(statement, 5);
        worktree.RepositoryIdentityDigest = ColumnText(statement, 6);
        worktree.WorktreePath = ColumnText(statement, 7);
        worktree.BranchRef = ColumnText(statement, 8);
        worktree.SourceHead = ColumnText(statement, 9);
        worktree.SourceReference = ColumnText(statement, 10);
        worktree.SnapshotDigest = ColumnText(statement, 11);
        worktree.BaselineCommit = ColumnText(statement, 12);
        worktree.OwnershipNonce = ColumnText(statement, 13);
        worktree.HasUndeliveredWork = sqlite3_column_int(statement, 14) != 0;
        worktree.CleanupEligible = sqlite3_column_int(statement, 15) != 0;
        worktree.PreservedPath = ColumnText(statement, 16);
        worktree.FailureReasonCode = ColumnText(statement, 17);
        worktree.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement, 18);
        worktree.UpdatedAtUnixMilliseconds = sqlite3_column_int64(statement, 19);
        return worktree;
    }

    TaskRunGitCheckpointRecord ReadTaskRunGitCheckpointRow(sqlite3_stmt* statement)
    {
        TaskRunGitCheckpointRecord checkpoint;
        checkpoint.Id = GitCheckpointId{ ColumnText(statement, 0) };
        checkpoint.Task = TaskId{ ColumnText(statement, 1) };
        checkpoint.Run = TaskRunId{ ColumnText(statement, 2) };
        checkpoint.Revision = static_cast<uint64_t>(sqlite3_column_int64(statement, 3));
        checkpoint.State = static_cast<TaskGitCheckpointState>(sqlite3_column_int(statement, 4));
        checkpoint.StartCommit = ColumnText(statement, 5);
        checkpoint.EndCommit = ColumnText(statement, 6);
        checkpoint.CheckpointCommit = ColumnText(statement, 7);
        checkpoint.AutoCheckpointed = sqlite3_column_int(statement, 8) != 0;
        checkpoint.ChangedPathsJson = ColumnText(statement, 9);
        checkpoint.DiffSummaryJson = ColumnText(statement, 10);
        checkpoint.PreservedPath = ColumnText(statement, 11);
        checkpoint.ReasonCode = ColumnText(statement, 12);
        checkpoint.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement, 13);
        checkpoint.UpdatedAtUnixMilliseconds = sqlite3_column_int64(statement, 14);
        return checkpoint;
    }

    TaskTerminalRecord ReadTaskTerminalRow(sqlite3_stmt* statement)
    {
        TaskTerminalRecord terminal;
        terminal.Id = TaskTerminalId{ ColumnText(statement, 0) };
        terminal.Task = TaskId{ ColumnText(statement, 1) };
        terminal.State = static_cast<TaskTerminalState>(sqlite3_column_int(statement, 2));
        terminal.Generation = static_cast<uint64_t>(sqlite3_column_int64(statement, 3));
        terminal.CurrentAgentSessionId = _optionalId<AgentSessionId>(statement, 4);
        terminal.UpdatedAtUnixMilliseconds = sqlite3_column_int64(statement, 5);
        return terminal;
    }

    AgentSessionRecord ReadAgentSessionRow(sqlite3_stmt* statement)
    {
        AgentSessionRecord session;
        session.Id = AgentSessionId{ ColumnText(statement, 0) };
        session.Task = TaskId{ ColumnText(statement, 1) };
        session.Provider = ColumnText(statement, 2);
        session.ProviderSessionId = ColumnText(statement, 3);
        session.State = static_cast<AgentSessionState>(sqlite3_column_int(statement, 4));
        session.IsCurrent = sqlite3_column_int(statement, 5) != 0;
        session.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement, 6);
        session.UpdatedAtUnixMilliseconds = sqlite3_column_int64(statement, 7);
        return session;
    }

    TaskRunRecord ReadTaskRunRow(sqlite3_stmt* statement)
    {
        TaskRunRecord run;
        run.Id = TaskRunId{ ColumnText(statement, 0) };
        run.Task = TaskId{ ColumnText(statement, 1) };
        run.ExecutionContext = ExecutionContextId{ ColumnText(statement, 2) };
        run.AgentSession = _optionalId<AgentSessionId>(statement, 3);
        run.RetryOfRunId = _optionalId<TaskRunId>(statement, 4);
        run.Trigger = static_cast<TaskRunTrigger>(sqlite3_column_int(statement, 5));
        run.State = static_cast<TaskRunState>(sqlite3_column_int(statement, 6));
        run.Sequence = static_cast<uint64_t>(sqlite3_column_int64(statement, 7));
        run.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement, 8);
        run.StartedAtUnixMilliseconds = ColumnOptionalInt64(statement, 9);
        run.FinishedAtUnixMilliseconds = ColumnOptionalInt64(statement, 10);
        run.Result = ColumnText(statement, 11);
        run.ReasonCode = ColumnText(statement, 12);
        return run;
    }

    TaskMessageRecord ReadTaskMessageRow(sqlite3_stmt* statement)
    {
        TaskMessageRecord message;
        message.Id = TaskMessageId{ ColumnText(statement, 0) };
        message.Task = TaskId{ ColumnText(statement, 1) };
        message.Author = static_cast<TaskMessageAuthor>(sqlite3_column_int(statement, 2));
        message.State = static_cast<TaskMessageState>(sqlite3_column_int(statement, 3));
        message.Revision = static_cast<uint64_t>(sqlite3_column_int64(statement, 4));
        message.Content = ColumnText(statement, 5);
        message.ClaimedByRunId = _optionalId<TaskRunId>(statement, 6);
        message.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement, 7);
        message.UpdatedAtUnixMilliseconds = sqlite3_column_type(statement, 8) == SQLITE_NULL ?
                                                        message.CreatedAtUnixMilliseconds :
                                                        sqlite3_column_int64(statement, 8);
        return message;
    }

    TaskRunMessageRecord ReadTaskRunMessageRow(sqlite3_stmt* statement)
    {
        TaskRunMessageRecord message;
        message.Run = TaskRunId{ ColumnText(statement, 0) };
        message.Message = TaskMessageId{ ColumnText(statement, 1) };
        message.Position = static_cast<uint32_t>(sqlite3_column_int64(statement, 2));
        message.MessageRevision = static_cast<uint64_t>(sqlite3_column_int64(statement, 3));
        message.CanonicalContent = ColumnText(statement, 4);
        return message;
    }

    TaskEventRecord ReadTaskEventRow(sqlite3_stmt* statement)
    {
        TaskEventRecord event;
        event.Id = TaskEventId{ ColumnText(statement, 0) };
        event.Task = TaskId{ ColumnText(statement, 1) };
        event.RunId = _optionalId<TaskRunId>(statement, 2);
        event.AggregateSequence = static_cast<uint64_t>(sqlite3_column_int64(statement, 3));
        const auto runSequence = ColumnOptionalInt64(statement, 4);
        event.RunSequence = runSequence ? std::optional<uint64_t>{ static_cast<uint64_t>(runSequence.value()) } : std::nullopt;
        event.Type = ColumnText(statement, 5);
        event.PayloadJson = ColumnText(statement, 6);
        event.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement, 7);
        return event;
    }

    TaskIntentRecord ReadTaskIntentRow(sqlite3_stmt* statement)
    {
        TaskIntentRecord intent;
        intent.Id = TaskIntentId{ ColumnText(statement, 0) };
        intent.Task = TaskId{ ColumnText(statement, 1) };
        intent.Kind = static_cast<TaskIntentKind>(sqlite3_column_int(statement, 2));
        intent.Stage = static_cast<TaskIntentStage>(sqlite3_column_int(statement, 3));
        intent.Revision = static_cast<uint64_t>(sqlite3_column_int64(statement, 4));
        intent.DeadlineAtUnixMilliseconds = ColumnOptionalInt64(statement, 5);
        intent.DetailJson = ColumnText(statement, 6);
        intent.ReasonCode = ColumnText(statement, 7);
        intent.CreatedAtUnixMilliseconds = sqlite3_column_int64(statement, 8);
        intent.UpdatedAtUnixMilliseconds = sqlite3_column_int64(statement, 9);
        return intent;
    }

    DeleteTombstoneRecord ReadDeleteTombstoneRow(sqlite3_stmt* statement)
    {
        DeleteTombstoneRecord tombstone;
        tombstone.Task = TaskId{ ColumnText(statement, 0) };
        tombstone.DeletedAtUnixMilliseconds = sqlite3_column_int64(statement, 1);
        tombstone.ExpiresAtUnixMilliseconds = sqlite3_column_int64(statement, 2);
        tombstone.ReplayDigest = ColumnText(statement, 3);
        tombstone.Command = _optionalId<CommandId>(statement, 4);
        const auto expectedVersion = ColumnOptionalInt64(statement, 5);
        const auto committedVersion = ColumnOptionalInt64(statement, 6);
        const auto lastEventSequence = ColumnOptionalInt64(statement, 7);
        tombstone.ExpectedTaskVersion = expectedVersion ?
                                                   std::optional<uint64_t>{ static_cast<uint64_t>(expectedVersion.value()) } :
                                                   std::nullopt;
        tombstone.CommittedTaskVersion = committedVersion ?
                                                    std::optional<uint64_t>{ static_cast<uint64_t>(committedVersion.value()) } :
                                                    std::nullopt;
        tombstone.LastEventSequence = lastEventSequence ?
                                                  std::optional<uint64_t>{ static_cast<uint64_t>(lastEventSequence.value()) } :
                                                  std::nullopt;
        return tombstone;
    }
}
