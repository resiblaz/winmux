// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentCatalogJson.h"
#include "ProtocolValidation.h"
#include "ProtocolVersion.h"
#include <limits>
#include <unordered_set>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        constexpr uint64_t MaximumVersion = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
        bool _text(const std::string& value, const size_t size, const bool required = false)
        { return value.size() <= size && (!required || !value.empty()) && value.find('\0') == std::string::npos && IsValidUtf8(value); }
        bool _id(const std::optional<std::string>& value) { return !value || IsCanonicalProtocolUuid(*value); }
        bool _uint(const Json::Value& value) { return value.isUInt64() && value.asUInt64() <= MaximumVersion; }
        bool _exact(const Json::Value& value, const std::initializer_list<std::string_view> keys)
        { return value.isObject() && value.size() == keys.size() && std::ranges::all_of(keys, [&](const auto key) { return value.isMember(std::string{ key }); }); }
        Json::Value _optional(const std::optional<std::string>& value) { return value ? Json::Value{ *value } : Json::Value{}; }
        bool _optionalString(const Json::Value& value, std::optional<std::string>& result)
        {
            if (value.isNull()) { result.reset(); return true; }
            if (!value.isString()) return false;
            result = value.asString();
            return true;
        }
        bool _validConfig(const AgentProfileConfiguration& c)
        { return _text(c.Name, 1024, true) && _text(c.Description, 1024) && c.Provider == "codex" && _text(c.Model, 256) && _text(c.Instructions, MaximumAgentInstructionsBytes); }
        Json::Value _config(const AgentProfileConfiguration& c)
        {
            Json::Value value{ Json::objectValue };
            value["name"] = c.Name; value["description"] = c.Description; value["provider"] = c.Provider;
            value["model"] = c.Model; value["instructions"] = c.Instructions;
            return value;
        }
        bool _readConfig(const Json::Value& value, AgentProfileConfiguration& c)
        {
            if (!_exact(value, { "name", "description", "provider", "model", "instructions" }) ||
                !value["name"].isString() || !value["description"].isString() || !value["provider"].isString() ||
                !value["model"].isString() || !value["instructions"].isString()) return false;
            c = { value["name"].asString(), value["description"].asString(), value["provider"].asString(), value["model"].asString(), value["instructions"].asString() };
            return _validConfig(c);
        }
        template<typename T>
        ProtocolRpcJsonDecodeResult<T> _invalid()
        { ProtocolRpcJsonDecodeResult<T> result; result.Status = ProtocolRpcJsonStatus::InvalidSchema; result.Error = "Agent payload schema is invalid"; return result; }
        template<typename T>
        bool _root(std::string_view payload, Json::Value& root, ProtocolRpcJsonDecodeResult<T>& result)
        {
            if (payload.size() > MaximumControlFrameBytes || !IsValidUtf8(payload))
            { result.Status = payload.size() > MaximumControlFrameBytes ? ProtocolRpcJsonStatus::Oversized : ProtocolRpcJsonStatus::InvalidUtf8; result.Error = "Agent JSON exceeds protocol bounds"; return false; }
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false; builder["allowTrailingCommas"] = false;
            builder["failIfExtra"] = true; builder["rejectDupKeys"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            std::string error;
            if (!reader || !reader->parse(payload.data(), payload.data() + payload.size(), &root, &error) || !root.isObject())
            { result.Status = ProtocolRpcJsonStatus::MalformedJson; result.Error = "Agent JSON is malformed"; return false; }
            return true;
        }
        template<typename TDecode>
        ProtocolRpcJsonEncodeResult _encode(const Json::Value& root, TDecode decode)
        {
            Json::StreamWriterBuilder writer;
            writer["emitUTF8"] = true; writer["indentation"] = ""; writer["commentStyle"] = "None";
            auto payload = Json::writeString(writer, root);
            const auto checked = decode(payload);
            if (!checked.Succeeded()) return { false, {}, checked.Error };
            return { true, std::move(payload), {} };
        }
    }

    bool ValidateAgentCatalogQuery(const AgentCatalogQueryRequest& value, std::string& error)
    {
        if (!_id(value.AgentId) || !_id(value.WorkspaceId) || !value.Limit || value.Limit > MaximumAgentCatalogPageSize ||
            (!value.AfterId.empty() && (!IsCanonicalProtocolUuid(value.AfterId) || value.AgentId)))
        { error = "Agent query is invalid"; return false; }
        return true;
    }
    bool ValidateAgentCatalogCommand(const AgentCatalogCommandRequest& value, std::string& error)
    {
        const auto isDefault = value.Operation == AgentCatalogOperation::SetWorkspaceDefault;
        const auto content = value.Operation == AgentCatalogOperation::Create || value.Operation == AgentCatalogOperation::Update;
        if (!IsCanonicalProtocolUuid(value.CommandId) || value.Operation > AgentCatalogOperation::SetWorkspaceDefault ||
            value.ExpectedVersion >= MaximumVersion || content != value.Configuration.has_value() ||
            (value.Configuration && !_validConfig(*value.Configuration)) ||
            (isDefault ? (!value.WorkspaceId || !_id(value.WorkspaceId) || (!value.AgentId.empty() && !IsCanonicalProtocolUuid(value.AgentId))) :
                         (value.WorkspaceId.has_value() || !IsCanonicalProtocolUuid(value.AgentId))) ||
            (value.Operation == AgentCatalogOperation::Create && value.ExpectedVersion) ||
            (!isDefault && value.Operation != AgentCatalogOperation::Create && !value.ExpectedVersion))
        { error = "Agent command is invalid"; return false; }
        return true;
    }
    bool ValidateTaskAgentConfiguration(const TaskAgentConfiguration& value, std::string& error)
    {
        if (value.Provider != "codex" || !_text(value.Model, 256) || !_text(value.Instructions, MaximumAgentInstructionsBytes) ||
            (value.Agent && (!IsCanonicalProtocolUuid(value.Agent->Id) || !value.Agent->Revision || value.Agent->Revision > MaximumVersion ||
                             !value.Model.empty() || !value.Instructions.empty())))
        { error = "Task Agent selection or overrides are invalid"; return false; }
        return true;
    }

    Json::Value details::TaskAgentConfigurationJson(const TaskAgentConfiguration& value)
    {
        Json::Value root{ Json::objectValue };
        root["agentId"] = value.Agent ? Json::Value{ value.Agent->Id } : Json::Value{};
        root["agentRevision"] = Json::UInt64{ value.Agent ? value.Agent->Revision : 0 };
        root["provider"] = value.Provider; root["model"] = value.Model; root["instructions"] = value.Instructions;
        return root;
    }
    bool details::ReadTaskAgentConfigurationJson(const Json::Value& value, TaskAgentConfiguration& result)
    {
        if (!_exact(value, { "agentId", "agentRevision", "provider", "model", "instructions" }) ||
            (!value["agentId"].isNull() && !value["agentId"].isString()) || !_uint(value["agentRevision"]) ||
            !value["provider"].isString() || !value["model"].isString() || !value["instructions"].isString()) return false;
        if (value["agentId"].isString()) result.Agent = AgentProfileReference{ value["agentId"].asString(), value["agentRevision"].asUInt64() };
        else if (value["agentRevision"].asUInt64()) return false;
        result.Provider = value["provider"].asString(); result.Model = value["model"].asString(); result.Instructions = value["instructions"].asString();
        std::string error;
        return ValidateTaskAgentConfiguration(result, error);
    }

    ProtocolRpcJsonDecodeResult<AgentCatalogQueryRequest> DecodeAgentCatalogQueryRequest(const std::string_view payload)
    {
        auto result = _invalid<AgentCatalogQueryRequest>(); Json::Value root;
        if (!_root(payload, root, result)) return result;
        if (!_exact(root, { "agentId", "workspaceId", "afterId", "includeArchived", "limit" }) ||
            !_optionalString(root["agentId"], result.Value.AgentId) || !_optionalString(root["workspaceId"], result.Value.WorkspaceId) ||
            !root["afterId"].isString() || !root["includeArchived"].isBool() || !_uint(root["limit"]) || root["limit"].asUInt64() > MaximumAgentCatalogPageSize) return result;
        result.Value.AfterId = root["afterId"].asString(); result.Value.IncludeArchived = root["includeArchived"].asBool(); result.Value.Limit = root["limit"].asUInt();
        if (ValidateAgentCatalogQuery(result.Value, result.Error)) { result.Status = ProtocolRpcJsonStatus::Succeeded; result.Error.clear(); }
        return result;
    }
    ProtocolRpcJsonEncodeResult EncodeAgentCatalogQueryRequest(const AgentCatalogQueryRequest& value)
    {
        Json::Value root{ Json::objectValue };
        root["agentId"] = _optional(value.AgentId); root["workspaceId"] = _optional(value.WorkspaceId);
        root["afterId"] = value.AfterId; root["includeArchived"] = value.IncludeArchived; root["limit"] = Json::UInt64{ value.Limit };
        return _encode(root, DecodeAgentCatalogQueryRequest);
    }

    ProtocolRpcJsonDecodeResult<AgentCatalogCommandRequest> DecodeAgentCatalogCommandRequest(const std::string_view payload)
    {
        auto result = _invalid<AgentCatalogCommandRequest>(); Json::Value root;
        if (!_root(payload, root, result)) return result;
        if (!_exact(root, { "commandId", "operation", "agentId", "workspaceId", "expectedVersion", "configuration" }) ||
            !root["commandId"].isString() || !_uint(root["operation"]) || root["operation"].asUInt64() > 4 || !root["agentId"].isString() ||
            !_optionalString(root["workspaceId"], result.Value.WorkspaceId) || !_uint(root["expectedVersion"])) return result;
        result.Value.CommandId = root["commandId"].asString(); result.Value.AgentId = root["agentId"].asString();
        result.Value.Operation = static_cast<AgentCatalogOperation>(root["operation"].asUInt()); result.Value.ExpectedVersion = root["expectedVersion"].asUInt64();
        if (!root["configuration"].isNull()) { result.Value.Configuration.emplace(); if (!_readConfig(root["configuration"], *result.Value.Configuration)) return result; }
        if (ValidateAgentCatalogCommand(result.Value, result.Error)) { result.Status = ProtocolRpcJsonStatus::Succeeded; result.Error.clear(); }
        return result;
    }
    ProtocolRpcJsonEncodeResult EncodeAgentCatalogCommandRequest(const AgentCatalogCommandRequest& value)
    {
        Json::Value root{ Json::objectValue };
        root["commandId"] = value.CommandId; root["operation"] = static_cast<uint32_t>(value.Operation); root["agentId"] = value.AgentId;
        root["workspaceId"] = _optional(value.WorkspaceId); root["expectedVersion"] = Json::UInt64{ value.ExpectedVersion };
        root["configuration"] = value.Configuration ? _config(*value.Configuration) : Json::Value{};
        return _encode(root, DecodeAgentCatalogCommandRequest);
    }

    ProtocolRpcJsonDecodeResult<AgentCatalogCommandResult> DecodeAgentCatalogCommandResult(const std::string_view payload)
    {
        auto result = _invalid<AgentCatalogCommandResult>(); Json::Value root;
        if (!_root(payload, root, result)) return result;
        if (!_exact(root, { "commandId", "agentId", "version", "revision", "eventSequence", "replayed", "affectedDefaults" }) ||
            !root["commandId"].isString() || !root["agentId"].isString() || !_uint(root["version"]) || !_uint(root["revision"]) ||
            !_uint(root["eventSequence"]) || !root["replayed"].isBool() || !root["affectedDefaults"].isUInt()) return result;
        auto& v = result.Value;
        v.CommandId = root["commandId"].asString(); v.AgentId = root["agentId"].asString(); v.Version = root["version"].asUInt64();
        v.Revision = root["revision"].asUInt64(); v.EventSequence = root["eventSequence"].asUInt64();
        v.Replayed = root["replayed"].asBool(); v.AffectedDefaults = root["affectedDefaults"].asUInt();
        if (!IsCanonicalProtocolUuid(v.CommandId) || (!v.AgentId.empty() && !IsCanonicalProtocolUuid(v.AgentId)) || !v.Version || !v.EventSequence) return result;
        result.Status = ProtocolRpcJsonStatus::Succeeded; result.Error.clear(); return result;
    }
    ProtocolRpcJsonEncodeResult EncodeAgentCatalogCommandResult(const AgentCatalogCommandResult& value)
    {
        Json::Value root{ Json::objectValue };
        root["commandId"] = value.CommandId; root["agentId"] = value.AgentId; root["version"] = Json::UInt64{ value.Version };
        root["revision"] = Json::UInt64{ value.Revision }; root["eventSequence"] = Json::UInt64{ value.EventSequence };
        root["replayed"] = value.Replayed; root["affectedDefaults"] = value.AffectedDefaults;
        return _encode(root, DecodeAgentCatalogCommandResult);
    }

    ProtocolRpcJsonDecodeResult<AgentCatalogQueryResult> DecodeAgentCatalogQueryResult(const std::string_view payload)
    {
        auto result = _invalid<AgentCatalogQueryResult>(); Json::Value root;
        if (!_root(payload, root, result)) return result;
        if (!_exact(root, { "profiles", "default", "nextAfterId", "generation" }) || !root["profiles"].isArray() ||
            root["profiles"].size() > MaximumAgentCatalogPageSize || !root["nextAfterId"].isString() || !_uint(root["generation"])) return result;
        result.Value.NextAfterId = root["nextAfterId"].asString(); result.Value.Generation = root["generation"].asUInt64();
        if (!result.Value.NextAfterId.empty() && !IsCanonicalProtocolUuid(result.Value.NextAfterId)) return result;
        std::unordered_set<std::string> ids;
        for (const auto& value : root["profiles"])
        {
            if (!_exact(value, { "id", "version", "revision", "configuration", "archived", "createdAt", "updatedAt", "activeRuns", "defaultWorkspaces" }) ||
                !value["id"].isString() || !_uint(value["version"]) || !_uint(value["revision"]) || !value["archived"].isBool() ||
                !value["createdAt"].isInt64() || !value["updatedAt"].isInt64() || !value["activeRuns"].isUInt() ||
                !value["defaultWorkspaces"].isArray() || value["defaultWorkspaces"].size() > 4096) return result;
            AgentProfileProjection profile;
            profile.Id = value["id"].asString(); profile.Version = value["version"].asUInt64(); profile.Revision = value["revision"].asUInt64();
            profile.Archived = value["archived"].asBool(); profile.CreatedAtUnixMilliseconds = value["createdAt"].asInt64();
            profile.UpdatedAtUnixMilliseconds = value["updatedAt"].asInt64(); profile.ActiveRuns = value["activeRuns"].asUInt();
            if (!_readConfig(value["configuration"], profile.Configuration) || !IsCanonicalProtocolUuid(profile.Id) ||
                !profile.Version || !profile.Revision || profile.Revision > profile.Version || !ids.insert(profile.Id).second ||
                profile.CreatedAtUnixMilliseconds < 0 || profile.UpdatedAtUnixMilliseconds < profile.CreatedAtUnixMilliseconds) return result;
            for (const auto& workspace : value["defaultWorkspaces"])
            { if (!workspace.isString() || !IsCanonicalProtocolUuid(workspace.asString())) return result; profile.DefaultWorkspaces.push_back(workspace.asString()); }
            result.Value.Profiles.emplace_back(std::move(profile));
        }
        if (!root["default"].isNull())
        {
            const auto& value = root["default"];
            WorkspaceAgentDefaultProjection def;
            if (!_exact(value, { "workspaceId", "agentId", "version" }) || !value["workspaceId"].isString() ||
                !_optionalString(value["agentId"], def.AgentId) || !_uint(value["version"])) return result;
            def.WorkspaceId = value["workspaceId"].asString(); def.Version = value["version"].asUInt64();
            if (!IsCanonicalProtocolUuid(def.WorkspaceId) || !_id(def.AgentId) || (def.AgentId && !def.Version)) return result;
            result.Value.Default = std::move(def);
        }
        result.Status = ProtocolRpcJsonStatus::Succeeded; result.Error.clear(); return result;
    }
    ProtocolRpcJsonEncodeResult EncodeAgentCatalogQueryResult(const AgentCatalogQueryResult& value)
    {
        if (value.Profiles.size() > MaximumAgentCatalogPageSize) return { false, {}, "Agent page exceeds protocol bounds" };
        Json::Value root{ Json::objectValue };
        root["profiles"] = Json::Value{ Json::arrayValue }; root["default"] = Json::Value{};
        root["nextAfterId"] = value.NextAfterId; root["generation"] = Json::UInt64{ value.Generation };
        for (const auto& profile : value.Profiles)
        {
            Json::Value row{ Json::objectValue };
            row["id"] = profile.Id; row["version"] = Json::UInt64{ profile.Version }; row["revision"] = Json::UInt64{ profile.Revision };
            row["configuration"] = _config(profile.Configuration); row["archived"] = profile.Archived;
            row["createdAt"] = Json::Int64{ profile.CreatedAtUnixMilliseconds }; row["updatedAt"] = Json::Int64{ profile.UpdatedAtUnixMilliseconds };
            row["activeRuns"] = profile.ActiveRuns; row["defaultWorkspaces"] = Json::Value{ Json::arrayValue };
            for (const auto& workspace : profile.DefaultWorkspaces) row["defaultWorkspaces"].append(workspace);
            root["profiles"].append(std::move(row));
        }
        if (value.Default)
        {
            root["default"]["workspaceId"] = value.Default->WorkspaceId; root["default"]["agentId"] = _optional(value.Default->AgentId);
            root["default"]["version"] = Json::UInt64{ value.Default->Version };
        }
        return _encode(root, DecodeAgentCatalogQueryResult);
    }
}
