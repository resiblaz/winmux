// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "SqliteTaskStoreInternal.h"
#include "TaskStoreProjectSqlite.h"

namespace Microsoft::Terminal::TaskCore
{
    ProjectCommandResult SqliteTaskStore::QueryProjectCommandReceipt(const CommandId& command, std::string_view digest) const
    {
        ProjectCommandResult result;
        if (!IsReady()) { result.Status = TaskStoreCommandStatus::NotReady; result.Error = LastError(); return result; }
        if (!IsCanonicalUuid(command.Value()) || digest.size() != 64)
        { result.Status = TaskStoreCommandStatus::InvalidCommand; result.Error = "project_invalid_receipt_query"; return result; }
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        auto db = _impl->Database;
        details::SqliteStatement receipt{ db, "SELECT payload_hash,subject_id,version,event_sequence,existing FROM project_command_receipt WHERE command_id=?1;", result.Error };
        if (!receipt || !details::BindText(db, receipt.Get(), 1, command.Value(), result.Error)) return result;
        const auto step = sqlite3_step(receipt.Get());
        if (step == SQLITE_DONE) { result.Status = TaskStoreCommandStatus::NotFound; return result; }
        if (step != SQLITE_ROW) { result.Error = details::SqliteError(db, step, "read project receipt"); return result; }
        if (details::ColumnText(receipt.Get(), 0) != digest)
        { result.Status = TaskStoreCommandStatus::IdempotencyConflict; result.Error = "project_command_reused"; return result; }
        result.Id = WorkspaceId{ details::ColumnText(receipt.Get(), 1) };
        result.Version = sqlite3_column_int64(receipt.Get(), 2); result.EventSequence = sqlite3_column_int64(receipt.Get(), 3);
        result.Existing = sqlite3_column_int(receipt.Get(), 4) != 0; result.Status = TaskStoreCommandStatus::Replayed;
        return result;
    }

    ProjectCommandResult SqliteTaskStore::ExecuteProjectCommand(const ProjectCommand& request)
    {
        ProjectCommandResult result;
        const auto fail = [&](TaskStoreCommandStatus status, std::string reason) { result.Status = status; result.Error = std::move(reason); return result; };
        if (!IsReady()) return fail(TaskStoreCommandStatus::NotReady, LastError());
        auto command = request;
        if (!NormalizeProjectCommand(command, result.Error)) { result.Status = TaskStoreCommandStatus::InvalidCommand; return result; }
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        auto db = _impl->Database;
        details::SqliteTransaction transaction{ db, result.Error };
        if (!transaction) return result;
        {
            details::SqliteStatement receipt{ db, "SELECT payload_hash,requested_id,subject_id,kind,version,event_sequence,existing FROM project_command_receipt WHERE command_id=?1;", result.Error };
            if (!receipt || !details::BindText(db, receipt.Get(), 1, command.Command.Value(), result.Error)) return result;
            const auto step = sqlite3_step(receipt.Get());
            if (step == SQLITE_ROW)
            {
                if (details::ColumnText(receipt.Get(), 0) != command.PayloadHash || details::ColumnText(receipt.Get(), 1) != command.Id.Value() ||
                    sqlite3_column_int(receipt.Get(), 3) != static_cast<int>(command.Kind))
                    return fail(TaskStoreCommandStatus::IdempotencyConflict, "project_command_reused");
                result.Id = WorkspaceId{ details::ColumnText(receipt.Get(), 2) };
                result.Version = sqlite3_column_int64(receipt.Get(), 4);
                result.EventSequence = sqlite3_column_int64(receipt.Get(), 5);
                result.Existing = sqlite3_column_int(receipt.Get(), 6) != 0;
                result.Status = TaskStoreCommandStatus::Replayed;
                return result;
            }
            if (step != SQLITE_DONE) return fail(TaskStoreCommandStatus::StorageError, details::SqliteError(db, step, "read project receipt"));
        }
        result.Id = command.Id;
        std::optional<ProjectRecord> current;
        if (!details::ReadProject(db, command.Id, current, result.Error)) return result;
        if (command.Kind == ProjectCommandKind::Create)
        {
            if (command.ExpectedVersion || current) return fail(TaskStoreCommandStatus::VersionConflict, "project_identity_exists");
            details::SqliteStatement duplicate{ db, "SELECT workspace_id FROM project WHERE binding_state=1 AND root_identity=?1 ORDER BY archived_at IS NOT NULL,workspace_id LIMIT 1;", result.Error };
            if (!duplicate || !details::BindText(db, duplicate.Get(), 1, command.RootIdentity, result.Error)) return result;
            const auto step = sqlite3_step(duplicate.Get());
            if (step == SQLITE_ROW)
            {
                result.Id = WorkspaceId{ details::ColumnText(duplicate.Get(), 0) };
                if (!details::ReadProject(db, result.Id, current, result.Error)) return result;
                result.Existing = true;
            }
            else if (step != SQLITE_DONE) return fail(TaskStoreCommandStatus::StorageError, details::SqliteError(db, step, "find project directory"));
        }
        if (command.Kind == ProjectCommandKind::RegisterLegacy && current)
        {
            // Reconnecting a view cannot rename or rebind an already registered project.
            result.Existing = true;
        }
        if (!current)
        {
            if (command.Kind != ProjectCommandKind::Create && command.Kind != ProjectCommandKind::RegisterLegacy)
                return fail(TaskStoreCommandStatus::NotFound, "project_not_found");
            if (command.ExpectedVersion) return fail(TaskStoreCommandStatus::VersionConflict, "project_version_conflict");
            const auto bound = command.Kind == ProjectCommandKind::Create;
            details::SqliteStatement insert{ db, R"SQL(
INSERT INTO project(workspace_id,name,description,kind,binding_state,root_path,root_identity,legacy,created_at,updated_at)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?9);
)SQL", result.Error };
            if (!insert || !details::BindText(db, insert.Get(), 1, command.Id.Value(), result.Error) ||
                !details::BindText(db, insert.Get(), 2, command.Name, result.Error) ||
                !details::BindText(db, insert.Get(), 3, command.Description, result.Error) ||
                !details::BindInt64(db, insert.Get(), 4, static_cast<int64_t>(command.DirectoryKind), result.Error) ||
                !details::BindInt64(db, insert.Get(), 5, bound, result.Error) ||
                !details::BindText(db, insert.Get(), 6, bound ? command.RootPath : "", result.Error) ||
                !details::BindText(db, insert.Get(), 7, bound ? command.RootIdentity : "", result.Error) ||
                !details::BindInt64(db, insert.Get(), 8, !bound, result.Error) ||
                !details::BindInt64(db, insert.Get(), 9, command.TimestampUnixMilliseconds, result.Error) ||
                !details::StepDone(db, insert.Get(), result.Error)) return result;
            result.Version = 1;
        }
        else if (result.Existing)
        {
            result.Version = current->Version;
        }
        else
        {
            if (current->Version != command.ExpectedVersion) return fail(TaskStoreCommandStatus::VersionConflict, "project_version_conflict");
            if (command.TimestampUnixMilliseconds < current->UpdatedAtUnixMilliseconds) return fail(TaskStoreCommandStatus::InvalidCommand, "project_stale_timestamp");
            if (current->Archived && command.Kind != ProjectCommandKind::Restore)
                return fail(TaskStoreCommandStatus::InvalidTransition, "project_archived");
            if (command.Kind == ProjectCommandKind::Archive || command.Kind == ProjectCommandKind::BindDirectory)
            {
                bool work{};
                if (!details::ProjectHasWork(db, command.Id, work, result.Error)) return result;
                if (work) return fail(TaskStoreCommandStatus::ActiveRunExists, "project_has_pending_work");
            }
            if (command.Kind == ProjectCommandKind::BindDirectory)
            {
                details::SqliteStatement duplicate{ db, "SELECT EXISTS(SELECT 1 FROM project WHERE root_identity=?1 AND workspace_id!=?2 AND binding_state=1);", result.Error };
                if (!duplicate || !details::BindText(db, duplicate.Get(), 1, command.RootIdentity, result.Error) ||
                    !details::BindText(db, duplicate.Get(), 2, command.Id.Value(), result.Error)) return result;
                const auto step = sqlite3_step(duplicate.Get());
                if (step != SQLITE_ROW) return fail(TaskStoreCommandStatus::StorageError, details::SqliteError(db, step, "check project binding"));
                if (sqlite3_column_int(duplicate.Get(), 0)) return fail(TaskStoreCommandStatus::VersionConflict, "project_directory_already_bound");
            }
            result.Version = current->Version + 1;
            const auto metadata = command.Kind == ProjectCommandKind::UpdateMetadata;
            const auto bind = command.Kind == ProjectCommandKind::BindDirectory;
            details::SqliteStatement update{ db, R"SQL(
UPDATE project SET version=?2,name=?3,description=?4,kind=?5,binding_state=?6,root_path=?7,root_identity=?8,
 archived_at=?9,legacy=?10,updated_at=?11 WHERE workspace_id=?1;
)SQL", result.Error };
            if (!update || !details::BindText(db, update.Get(), 1, command.Id.Value(), result.Error) ||
                !details::BindInt64(db, update.Get(), 2, static_cast<int64_t>(result.Version), result.Error) ||
                !details::BindText(db, update.Get(), 3, metadata ? command.Name : current->Name, result.Error) ||
                !details::BindText(db, update.Get(), 4, metadata ? command.Description : current->Description, result.Error) ||
                !details::BindInt64(db, update.Get(), 5, static_cast<int64_t>(bind ? command.DirectoryKind : current->Kind), result.Error) ||
                !details::BindInt64(db, update.Get(), 6, bind ? 1 : static_cast<int64_t>(current->Binding), result.Error) ||
                !details::BindText(db, update.Get(), 7, bind ? command.RootPath : current->RootPath, result.Error) ||
                !details::BindText(db, update.Get(), 8, bind ? command.RootIdentity : current->RootIdentity, result.Error) ||
                !details::BindOptionalInt64(db, update.Get(), 9, command.Kind == ProjectCommandKind::Archive ? std::optional{ command.TimestampUnixMilliseconds } : std::nullopt, result.Error) ||
                !details::BindInt64(db, update.Get(), 10, metadata || bind ? false : current->Legacy, result.Error) ||
                !details::BindInt64(db, update.Get(), 11, command.TimestampUnixMilliseconds, result.Error) ||
                !details::StepDone(db, update.Get(), result.Error)) return result;
        }
        const auto sequence = details::QueryInt64(db, "SELECT coalesce(max(sequence),0) FROM project_event;", result.Error);
        if (!sequence) return result;
        result.EventSequence = *sequence;
        details::SqliteStatement receipt{ db, "INSERT INTO project_command_receipt(command_id,payload_hash,requested_id,subject_id,kind,version,event_sequence,existing) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);", result.Error };
        if (!receipt || !details::BindText(db, receipt.Get(), 1, command.Command.Value(), result.Error) ||
            !details::BindText(db, receipt.Get(), 2, command.PayloadHash, result.Error) ||
            !details::BindText(db, receipt.Get(), 3, command.Id.Value(), result.Error) ||
            !details::BindText(db, receipt.Get(), 4, result.Id.Value(), result.Error) ||
            !details::BindInt64(db, receipt.Get(), 5, static_cast<int64_t>(command.Kind), result.Error) ||
            !details::BindInt64(db, receipt.Get(), 6, static_cast<int64_t>(result.Version), result.Error) ||
            !details::BindInt64(db, receipt.Get(), 7, static_cast<int64_t>(result.EventSequence), result.Error) ||
            !details::BindInt64(db, receipt.Get(), 8, result.Existing, result.Error) ||
            !details::StepDone(db, receipt.Get(), result.Error) || !transaction.Commit(result.Error)) return result;
        result.Status = TaskStoreCommandStatus::Succeeded;
        return result;
    }
}
