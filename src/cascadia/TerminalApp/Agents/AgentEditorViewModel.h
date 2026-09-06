// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "IAgentCatalogSource.h"

namespace winrt::TerminalApp::implementation::agents
{
    // UI-thread editor state. Catalog refreshes never overwrite a local draft.
    class AgentEditorViewModel
    {
    public:
        std::optional<protocol::AgentProfileProjection> Saved;
        protocol::AgentProfileConfiguration Draft;
        std::optional<protocol::AgentCatalogCommandRequest> Pending;
        bool Saving{};
        bool OutcomeUnknown{};

        void Load(protocol::AgentProfileProjection profile)
        { Draft = profile.Configuration; Saved = std::move(profile); Pending.reset(); Saving = OutcomeUnknown = false; }
        void New()
        { Saved.reset(); Draft = {}; Pending.reset(); Saving = OutcomeUnknown = false; }
        void Copy(std::string name)
        { Saved.reset(); Draft.Name = std::move(name); Pending.reset(); Saving = OutcomeUnknown = false; }
        bool Dirty() const noexcept
        {
            const protocol::AgentProfileConfiguration empty;
            const auto& saved = Saved ? Saved->Configuration : empty;
            return Draft.Name != saved.Name || Draft.Description != saved.Description || Draft.Provider != saved.Provider ||
                   Draft.Model != saved.Model || Draft.Instructions != saved.Instructions;
        }
        protocol::AgentCatalogCommandRequest Prepare(std::string commandId, std::string newId)
        {
            if (OutcomeUnknown && Pending) { Saving = true; return *Pending; }
            protocol::AgentCatalogCommandRequest command;
            command.CommandId = std::move(commandId);
            command.AgentId = Saved ? Saved->Id : std::move(newId);
            command.ExpectedVersion = Saved ? Saved->Version : 0;
            command.Operation = Saved ? protocol::AgentCatalogOperation::Update : protocol::AgentCatalogOperation::Create;
            command.Configuration = Draft;
            Pending = command; Saving = true;
            return command;
        }
        void Complete(const CatalogReply<protocol::AgentCatalogCommandResult>& reply)
        {
            if (reply.Succeeded && Pending && Pending->Operation != protocol::AgentCatalogOperation::SetWorkspaceDefault)
            {
                if (!Saved) Saved.emplace();
                Saved->Id = Pending->AgentId; Saved->Version = reply.Value.Version; Saved->Revision = reply.Value.Revision;
                if (Pending->Configuration) Saved->Configuration = *Pending->Configuration;
                Saved->Archived = Pending->Operation == protocol::AgentCatalogOperation::Archive;
                if (Saved->Archived) Saved->DefaultWorkspaces.clear();
            }
            Saving = false; OutcomeUnknown = reply.OutcomeUnknown;
            if (!OutcomeUnknown) Pending.reset();
        }
    };
}
