// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "ProjectCatalog.h"
#include "AgentProfile.h"
#include "TaskRecordValidation.h"
#include <limits>

namespace Microsoft::Terminal::TaskCore
{
    bool NormalizeProjectCommand(ProjectCommand& command, std::string& error)
    {
        if (!IsCanonicalUuid(command.Command.Value()) || !IsCanonicalUuid(command.Id.Value()) ||
            command.Kind > ProjectCommandKind::Restore || command.DirectoryKind > ProjectKind::Git ||
            command.ExpectedVersion >= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) ||
            command.TimestampUnixMilliseconds < 0 || command.Description.size() > MaximumProjectDescriptionBytes ||
            command.RootPath.size() > MaximumTaskPathBytes || command.PayloadHash.size() != 64 ||
            !std::ranges::all_of(command.PayloadHash, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        { error = "project_invalid_request"; return false; }
        if (command.Kind == ProjectCommandKind::Create || command.Kind == ProjectCommandKind::UpdateMetadata ||
            command.Kind == ProjectCommandKind::RegisterLegacy)
        {
            if (command.Kind == ProjectCommandKind::RegisterLegacy && command.Name.empty())
                command.Name = "Workspace " + command.Id.Value().substr(0, 8);
            AgentProfileContent content;
            content.Name = command.Name;
            content.Instructions = command.Description;
            if (!NormalizeAgentProfileContent(content, error)) return false;
            command.Name = std::move(content.Name);
        }
        if (command.Kind == ProjectCommandKind::Create || command.Kind == ProjectCommandKind::BindDirectory)
        {
            if (command.RootPath.empty() || command.RootPath.find('\0') != std::string::npos || command.RootIdentity.size() != 64 ||
                !std::ranges::all_of(command.RootIdentity, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.RootPath.data(), static_cast<int>(command.RootPath.size()), nullptr, 0) <= 0)
            { error = "project_directory_identity_required"; return false; }
        }
        return true;
    }
}
