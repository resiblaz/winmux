// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStoreInternal.h"
#include "TaskStoreAgentSqlite.h"
#include "TaskStoreSqlite.h"
#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        bool _revision(sqlite3* db, const AgentProfileCommand& command, const AgentProfileContent& content, const uint64_t revision, std::string& error)
        {
            details::SqliteStatement statement{ db, "INSERT INTO agent_profile_revision(agent_id,revision,name,description,provider,model,instructions,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);", error };
            return statement && details::BindText(db, statement.Get(), 1, command.Agent.Value(), error) &&
                   details::BindInt64(db, statement.Get(), 2, revision, error) &&
                   details::BindText(db, statement.Get(), 3, content.Name, error) &&
                   details::BindText(db, statement.Get(), 4, content.Description, error) &&
                   details::BindText(db, statement.Get(), 5, content.Provider, error) &&
                   details::BindText(db, statement.Get(), 6, content.Model, error) &&
                   details::BindText(db, statement.Get(), 7, content.Instructions, error) &&
                   details::BindInt64(db, statement.Get(), 8, command.TimestampUnixMilliseconds, error) &&
                   details::StepDone(db, statement.Get(), error);
        }
    }

    AgentProfileCommandResult SqliteTaskStore::ExecuteAgentProfileCommand(const AgentProfileCommand& command)
    {
        AgentProfileCommandResult result;
        const auto fail = [&](const TaskStoreCommandStatus status, std::string error) {
            result.Status = status; result.Error = std::move(error); return result;
        };
        if (!IsReady()) return fail(TaskStoreCommandStatus::NotReady, LastError());
        const auto isDefault = command.Kind == AgentProfileCommandKind::SetWorkspaceDefault;
        const auto hasContent = command.Kind == AgentProfileCommandKind::Create || command.Kind == AgentProfileCommandKind::Update;
        const auto maximum = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
        if (!IsCanonicalUuid(command.Command.Value()) || command.PayloadHash.size() != 64 ||
            !std::ranges::all_of(command.PayloadHash, [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
            command.Kind > AgentProfileCommandKind::SetWorkspaceDefault || command.TimestampUnixMilliseconds < 0 ||
            command.ExpectedVersion >= maximum || hasContent != command.Content.has_value() ||
            (isDefault ? (!command.Workspace || !IsCanonicalUuid(command.Workspace->Value()) ||
                          (!command.Agent.Empty() && !IsCanonicalUuid(command.Agent.Value()))) :
                         (command.Workspace.has_value() || !IsCanonicalUuid(command.Agent.Value()))) ||
            (command.Kind == AgentProfileCommandKind::Create && command.ExpectedVersion != 0) ||
            (!isDefault && command.Kind != AgentProfileCommandKind::Create && !command.ExpectedVersion))
            return fail(TaskStoreCommandStatus::InvalidCommand, "Agent command is invalid");
        auto content = command.Content;
        if (content && !NormalizeAgentProfileContent(*content, result.Error))
        { result.Status = TaskStoreCommandStatus::InvalidRecord; return result; }
        const auto subject = isDefault ? command.Workspace->Value() : command.Agent.Value();
        const std::scoped_lock lock{ _impl->DatabaseMutex };
        auto db = _impl->Database;
        details::SqliteTransaction transaction{ db, result.Error };
        if (!transaction) return result;
        {
            details::SqliteStatement receipt{ db, "SELECT payload_hash,kind,subject_id,version,revision,event_sequence,affected_defaults FROM agent_command_receipt WHERE command_id=?1;", result.Error };
            if (!receipt || !details::BindText(db, receipt.Get(), 1, command.Command.Value(), result.Error)) return result;
            const auto step = sqlite3_step(receipt.Get());
            if (step == SQLITE_ROW)
            {
                if (details::ColumnText(receipt.Get(), 0) != command.PayloadHash ||
                    sqlite3_column_int(receipt.Get(), 1) != static_cast<int>(command.Kind) || details::ColumnText(receipt.Get(), 2) != subject)
                    return fail(TaskStoreCommandStatus::IdempotencyConflict, "Agent command ID was reused with different input");
                result.Status = TaskStoreCommandStatus::Replayed;
                result.Version = sqlite3_column_int64(receipt.Get(), 3);
                result.Revision = sqlite3_column_int64(receipt.Get(), 4);
                result.EventSequence = sqlite3_column_int64(receipt.Get(), 5);
                result.AffectedDefaults = static_cast<uint32_t>(sqlite3_column_int64(receipt.Get(), 6));
                return result;
            }
            if (step != SQLITE_DONE) { result.Error = details::SqliteError(db, step, "read Agent receipt"); return result; }
        }
        std::optional<AgentProfileRecord> profile;
        if (!command.Agent.Empty() && !details::ReadAgentProfile(db, command.Agent, profile, result.Error)) return result;
        if (isDefault)
        {
            if (!command.Agent.Empty() && (!profile || profile->Archived))
                return fail(TaskStoreCommandStatus::InvalidTransition, "Workspace default requires an available Agent");
            if (profile && profile->DefaultWorkspaces.size() >= 4096 &&
                std::ranges::find(profile->DefaultWorkspaces, *command.Workspace) == profile->DefaultWorkspaces.end())
                return fail(TaskStoreCommandStatus::CapacityExceeded, "Agent workspace default capacity was reached");
            uint64_t version{};
            {
                details::SqliteStatement current{ db, "SELECT version FROM workspace_agent_default WHERE workspace_id=?1;", result.Error };
                if (!current || !details::BindText(db, current.Get(), 1, subject, result.Error)) return result;
                const auto step = sqlite3_step(current.Get());
                if (step == SQLITE_ROW) version = sqlite3_column_int64(current.Get(), 0);
                else if (step != SQLITE_DONE) { result.Error = details::SqliteError(db, step, "read workspace default"); return result; }
            }
            if (version != command.ExpectedVersion) return fail(TaskStoreCommandStatus::VersionConflict, "Workspace default changed; reload before saving");
            result.Version = version + 1;
            result.Revision = profile ? profile->Revision : 0;
            details::SqliteStatement update{ db, "INSERT INTO workspace_agent_default(workspace_id,agent_id,version) VALUES(?1,?2,?3) ON CONFLICT(workspace_id) DO UPDATE SET agent_id=excluded.agent_id,version=excluded.version;", result.Error };
            if (!update || !details::BindText(db, update.Get(), 1, subject, result.Error) ||
                !details::BindOptionalText(db, update.Get(), 2, command.Agent.Empty() ? std::nullopt : std::optional{ command.Agent.Value() }, result.Error) ||
                !details::BindInt64(db, update.Get(), 3, result.Version, result.Error) || !details::StepDone(db, update.Get(), result.Error)) return result;
        }
        else if (command.Kind == AgentProfileCommandKind::Create)
        {
            if (profile) return fail(TaskStoreCommandStatus::VersionConflict, "Agent identity already exists");
            result.Version = result.Revision = 1;
            details::SqliteStatement insert{ db, "INSERT INTO agent_profile(id,version,current_revision,created_at,updated_at) VALUES(?1,1,1,?2,?2);", result.Error };
            if (!insert || !details::BindText(db, insert.Get(), 1, subject, result.Error) ||
                !details::BindInt64(db, insert.Get(), 2, command.TimestampUnixMilliseconds, result.Error) ||
                !details::StepDone(db, insert.Get(), result.Error) || !_revision(db, command, *content, 1, result.Error)) return result;
        }
        else
        {
            if (!profile) return fail(TaskStoreCommandStatus::NotFound, "Agent was not found");
            if (profile->Version != command.ExpectedVersion) return fail(TaskStoreCommandStatus::VersionConflict, "Agent changed in another window; reload or save a copy");
            if (command.TimestampUnixMilliseconds < profile->UpdatedAtUnixMilliseconds || profile->Revision >= maximum)
                return fail(TaskStoreCommandStatus::InvalidCommand, "Agent timestamp or revision is invalid");
            if (command.Kind == AgentProfileCommandKind::Update && profile->Archived)
                return fail(TaskStoreCommandStatus::InvalidTransition, "Restore the Agent before editing it");
            result.Version = profile->Version + 1;
            result.Revision = profile->Revision + (command.Kind == AgentProfileCommandKind::Update ? 1 : 0);
            const auto archived = command.Kind == AgentProfileCommandKind::Archive;
            details::SqliteStatement update{ db, "UPDATE agent_profile SET version=?1,current_revision=?2,archived_at=?3,updated_at=?4 WHERE id=?5;", result.Error };
            if (!update || !details::BindInt64(db, update.Get(), 1, result.Version, result.Error) ||
                !details::BindInt64(db, update.Get(), 2, result.Revision, result.Error) ||
                !details::BindOptionalInt64(db, update.Get(), 3, archived ? std::optional{ command.TimestampUnixMilliseconds } : std::nullopt, result.Error) ||
                !details::BindInt64(db, update.Get(), 4, command.TimestampUnixMilliseconds, result.Error) ||
                !details::BindText(db, update.Get(), 5, subject, result.Error) || !details::StepDone(db, update.Get(), result.Error)) return result;
            if (content && !_revision(db, command, *content, result.Revision, result.Error)) return result;
            if (archived)
            {
                details::SqliteStatement clear{ db, "UPDATE workspace_agent_default SET agent_id=NULL,version=version+1 WHERE agent_id=?1;", result.Error };
                if (!clear || !details::BindText(db, clear.Get(), 1, subject, result.Error) || !details::StepDone(db, clear.Get(), result.Error)) return result;
                result.AffectedDefaults = static_cast<uint32_t>(sqlite3_changes(db));
            }
        }
        details::SqliteStatement event{ db, "INSERT INTO agent_event(agent_id,workspace_id,kind,version,created_at) VALUES(?1,?2,?3,?4,?5);", result.Error };
        if (!event || !details::BindOptionalText(db, event.Get(), 1, command.Agent.Empty() ? std::nullopt : std::optional{ command.Agent.Value() }, result.Error) ||
            !details::BindOptionalText(db, event.Get(), 2, command.Workspace ? std::optional{ command.Workspace->Value() } : std::nullopt, result.Error) ||
            !details::BindInt64(db, event.Get(), 3, static_cast<int64_t>(command.Kind), result.Error) ||
            !details::BindInt64(db, event.Get(), 4, result.Version, result.Error) ||
            !details::BindInt64(db, event.Get(), 5, command.TimestampUnixMilliseconds, result.Error) || !details::StepDone(db, event.Get(), result.Error)) return result;
        result.EventSequence = sqlite3_last_insert_rowid(db);
        details::SqliteStatement receipt{ db, "INSERT INTO agent_command_receipt(command_id,payload_hash,kind,subject_id,version,revision,event_sequence,affected_defaults) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);", result.Error };
        if (!receipt || !details::BindText(db, receipt.Get(), 1, command.Command.Value(), result.Error) ||
            !details::BindText(db, receipt.Get(), 2, command.PayloadHash, result.Error) ||
            !details::BindInt64(db, receipt.Get(), 3, static_cast<int64_t>(command.Kind), result.Error) ||
            !details::BindText(db, receipt.Get(), 4, subject, result.Error) ||
            !details::BindInt64(db, receipt.Get(), 5, result.Version, result.Error) ||
            !details::BindInt64(db, receipt.Get(), 6, result.Revision, result.Error) ||
            !details::BindInt64(db, receipt.Get(), 7, result.EventSequence, result.Error) ||
            !details::BindInt64(db, receipt.Get(), 8, result.AffectedDefaults, result.Error) ||
            !details::StepDone(db, receipt.Get(), result.Error) || !transaction.Commit(result.Error)) return result;
        result.Status = TaskStoreCommandStatus::Succeeded;
        return result;
    }
}
