// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../TerminalApp/Agents/AgentEditorViewModel.h"

using namespace WEX::TestExecution;
using namespace winrt::TerminalApp::implementation::agents;

namespace TerminalAppLocalTests
{
    class AgentEditorViewModelTests
    {
        TEST_CLASS(AgentEditorViewModelTests);
        TEST_METHOD(ConflictPreservesDraftAndSaveAsNewChangesIdentity);
        TEST_METHOD(UnknownOutcomeReusesRequestAndKeepsConfirmedCreateIdentity);
        TEST_METHOD(WorkspaceDefaultReceiptDoesNotChangeProfileVersion);
    };

    void AgentEditorViewModelTests::ConflictPreservesDraftAndSaveAsNewChangesIdentity()
    {
        AgentEditorViewModel model;
        protocol::AgentProfileProjection saved; saved.Id = "original"; saved.Version = 7; saved.Revision = 3;
        saved.Configuration = { "Reviewer", "Display", "codex", "", "Old instructions" };
        model.Load(saved); model.Draft.Instructions = "New draft";
        const auto update = model.Prepare("update-command", "unused");
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, update.ExpectedVersion);
        CatalogReply<protocol::AgentCatalogCommandResult> conflict; conflict.ErrorCode = protocol::ProtocolRpcErrorCode::Conflict;
        model.Complete(conflict);
        VERIFY_IS_TRUE(model.Dirty()); VERIFY_ARE_EQUAL(std::string{ "New draft" }, model.Draft.Instructions);
        model.Copy("Reviewer copy");
        const auto copy = model.Prepare("copy-command", "new-agent");
        VERIFY_IS_TRUE(copy.Operation == protocol::AgentCatalogOperation::Create);
        VERIFY_ARE_EQUAL(std::string{ "new-agent" }, copy.AgentId); VERIFY_ARE_EQUAL(uint64_t{}, copy.ExpectedVersion);
    }

    void AgentEditorViewModelTests::UnknownOutcomeReusesRequestAndKeepsConfirmedCreateIdentity()
    {
        AgentEditorViewModel model; model.Draft.Name = "Reviewer"; model.Draft.Instructions = "Original draft";
        const auto first = model.Prepare("first-command", "first-agent");
        CatalogReply<protocol::AgentCatalogCommandResult> unknown; unknown.OutcomeUnknown = true;
        model.Complete(unknown);
        const auto retry = model.Prepare("different-command", "different-agent");
        VERIFY_ARE_EQUAL(first.CommandId, retry.CommandId); VERIFY_ARE_EQUAL(first.AgentId, retry.AgentId);
        VERIFY_ARE_EQUAL(first.Configuration->Instructions, retry.Configuration->Instructions);
        CatalogReply<protocol::AgentCatalogCommandResult> applied; applied.Succeeded = true; applied.Value.Version = 1; applied.Value.Revision = 1;
        model.Complete(applied);
        VERIFY_IS_TRUE(model.Saved.has_value()); VERIFY_ARE_EQUAL(first.AgentId, model.Saved->Id);
        VERIFY_IS_FALSE(model.Dirty()); VERIFY_IS_FALSE(model.OutcomeUnknown);
        model.Draft.Instructions = "Later edit";
        const auto edit = model.Prepare("edit-command", "unused");
        VERIFY_IS_TRUE(edit.Operation == protocol::AgentCatalogOperation::Update); VERIFY_ARE_EQUAL(first.AgentId, edit.AgentId);
    }

    void AgentEditorViewModelTests::WorkspaceDefaultReceiptDoesNotChangeProfileVersion()
    {
        AgentEditorViewModel model; protocol::AgentProfileProjection saved; saved.Id = "profile"; saved.Version = 8; saved.Revision = 4;
        saved.Configuration.Name = "Reviewer"; model.Load(saved);
        protocol::AgentCatalogCommandRequest request; request.Operation = protocol::AgentCatalogOperation::SetWorkspaceDefault;
        model.Pending = request; model.Saving = true;
        CatalogReply<protocol::AgentCatalogCommandResult> applied; applied.Succeeded = true; applied.Value.Version = 2; applied.Value.Revision = 4;
        model.Complete(applied);
        VERIFY_ARE_EQUAL(uint64_t{ 8 }, model.Saved->Version); VERIFY_IS_FALSE(model.Saving);
    }
}
