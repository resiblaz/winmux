// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CodexAppServerEventParser.h"

#include <json/json.h>

namespace Microsoft::Terminal::TaskProvider
{
    namespace
    {
        constexpr size_t MaximumJsonDepth{ 32 };
        constexpr size_t MaximumJsonNodes{ 8192 };

        std::string _string(
            const Json::Value& value,
            const char* name,
            const size_t maximum = MaximumProviderIdentifierBytes)
        {
            if (!value.isObject() || !value.isMember(name) || !value[name].isString())
            {
                return {};
            }
            auto result = value[name].asString();
            if (result.size() > maximum || result.find('\0') != std::string::npos)
            {
                return {};
            }
            return result;
        }

        bool _sensitive(const std::string_view key)
        {
            std::string folded{ key };
            std::ranges::transform(folded, folded.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            const auto credentialToken = folded == "token" || folded == "accesstoken" ||
                                         folded == "idtoken" || folded == "refreshtoken" ||
                                         folded.ends_with("_token");
            return credentialToken || folded.find("secret") != std::string::npos ||
                   folded.find("authorization") != std::string::npos ||
                   folded.find("capability") != std::string::npos;
        }

        bool _sanitize(Json::Value& value, const size_t depth, size_t& nodes, bool& redacted, bool& truncated)
        {
            if (++nodes > MaximumJsonNodes || depth > MaximumJsonDepth)
            {
                value = "[truncated]";
                truncated = true;
                return false;
            }
            if (value.isObject())
            {
                for (const auto& name : value.getMemberNames())
                {
                    if (_sensitive(name))
                    {
                        value[name] = "[redacted]";
                        redacted = true;
                    }
                    else
                    {
                        _sanitize(value[name], depth + 1, nodes, redacted, truncated);
                    }
                }
            }
            else if (value.isArray())
            {
                if (value.size() > 1024)
                {
                    value.resize(1024);
                    truncated = true;
                }
                for (auto& item : value)
                {
                    _sanitize(item, depth + 1, nodes, redacted, truncated);
                }
            }
            else if (value.isString())
            {
                auto text = value.asString();
                if (text.size() > MaximumProviderTextBytes)
                {
                    text.resize(MaximumProviderTextBytes);
                    value = std::move(text);
                    truncated = true;
                }
            }
            return true;
        }

        std::string _json(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            builder["indentation"] = "";
            return Json::writeString(builder, value);
        }

        std::string _boundedPayload(const Json::Value& params, TaskProviderEvent& event)
        {
            auto sanitized = params;
            size_t nodes{};
            _sanitize(sanitized, 0, nodes, event.Redacted, event.Truncated);
            auto payload = _json(sanitized);
            event.OriginalBytes = payload.size();
            if (payload.size() <= MaximumProviderPayloadBytes)
            {
                return payload;
            }
            event.Truncated = true;
            Json::Value summary{ Json::objectValue };
            summary["truncated"] = true;
            summary["originalBytes"] = Json::UInt64{ event.OriginalBytes };
            return _json(summary);
        }

        std::string _itemType(const Json::Value& params)
        {
            return params.isMember("item") ? _string(params["item"], "type") : std::string{};
        }

        std::string _itemText(const Json::Value& params)
        {
            if (!params.isMember("item") || !params["item"].isObject())
            {
                return {};
            }
            const auto& item = params["item"];
            for (const auto name : { "text", "message", "command", "tool", "name", "query", "review" })
            {
                if (auto value = _string(item, name, MaximumProviderTextBytes); !value.empty())
                {
                    return value;
                }
            }
            return {};
        }

        TaskProviderEventKind _itemKind(const std::string_view type, const bool completed)
        {
            if (type == "agentMessage") return completed ? TaskProviderEventKind::AgentMessageCompleted : TaskProviderEventKind::ToolProgress;
            if (type == "fileChange") return TaskProviderEventKind::FileChanged;
            if (type == "plan") return TaskProviderEventKind::PlanUpdated;
            if (type == "commandExecution" || type == "mcpToolCall" || type == "dynamicToolCall" || type == "webSearch")
            {
                return completed ? TaskProviderEventKind::ToolCompleted : TaskProviderEventKind::ToolStarted;
            }
            return completed ? TaskProviderEventKind::ToolCompleted : TaskProviderEventKind::ToolStarted;
        }

        std::string _approvalId(const Json::Value& params)
        {
            if (auto id = _string(params, "approvalId"); !id.empty()) return id;
            return _string(params, "itemId");
        }
    }

    CodexEventParseResult ParseCodexAppServerEvent(
        const Json::Value& message,
        const uint64_t sequence) noexcept
    {
        CodexEventParseResult result;
        try
        {
            if (!message.isObject() || !message.isMember("method") ||
                !message["method"].isString() || !message.isMember("params") ||
                !message["params"].isObject() || sequence == 0)
            {
                result.Status = TaskProviderStatus::MalformedResponse;
                result.Error = "Codex event envelope is malformed";
                return result;
            }
            const auto method = message["method"].asString();
            if (method.empty() || method.size() > 128)
            {
                result.Status = TaskProviderStatus::MalformedResponse;
                result.Error = "Codex event method is invalid";
                return result;
            }
            const auto& params = message["params"];
            TaskProviderEvent event;
            event.Sequence = sequence;
            event.SessionId = _string(params, "threadId");
            event.RunId = _string(params, "turnId");
            event.ItemId = _string(params, "itemId");

            if (method == "item/commandExecution/requestApproval" ||
                method == "item/fileChange/requestApproval" ||
                method == "item/permissions/requestApproval")
            {
                if (!message.isMember("id") ||
                    (!message["id"].isUInt64() && !message["id"].isString()) ||
                    event.SessionId.empty() || event.RunId.empty())
                {
                    result.Status = TaskProviderStatus::MalformedResponse;
                    result.Error = "Codex approval request is malformed";
                    return result;
                }
                event.Kind = TaskProviderEventKind::ApprovalRequested;
                event.ApprovalId = _approvalId(params);
                event.ItemId = _string(params, "itemId");
                event.Text = _string(params, "reason", MaximumProviderTextBytes);
                if (event.Text.empty()) event.Text = _string(params, "command", MaximumProviderTextBytes);
                if (event.ApprovalId.empty())
                {
                    result.Status = TaskProviderStatus::MalformedResponse;
                    result.Error = "Codex approval identity is missing";
                    return result;
                }
                CodexPendingApproval approval;
                approval.RequestId = message["id"];
                approval.Method = method;
                approval.SessionId = event.SessionId;
                approval.RunId = event.RunId;
                approval.ApprovalId = event.ApprovalId;
                if (params.isMember("permissions")) approval.RequestedPermissions = params["permissions"];
                result.Approval = std::move(approval);
            }
            else if (method == "thread/started")
            {
                event.Kind = TaskProviderEventKind::SessionStarted;
                if (params.isMember("thread")) event.SessionId = _string(params["thread"], "id");
            }
            else if (method == "turn/started")
            {
                event.Kind = TaskProviderEventKind::RunStarted;
                if (params.isMember("turn")) event.RunId = _string(params["turn"], "id");
            }
            else if (method == "item/agentMessage/delta")
            {
                event.Kind = TaskProviderEventKind::AgentMessageDelta;
                event.Text = _string(params, "delta", MaximumProviderTextBytes);
            }
            else if (method == "item/started" || method == "item/completed")
            {
                const auto completed = method == "item/completed";
                event.Kind = _itemKind(_itemType(params), completed);
                if (params.isMember("item")) event.ItemId = _string(params["item"], "id");
                event.Text = _itemText(params);
            }
            else if (method == "item/commandExecution/outputDelta" || method == "item/mcpToolCall/progress")
            {
                event.Kind = TaskProviderEventKind::ToolProgress;
                event.Text = _string(params, method == "item/mcpToolCall/progress" ? "message" : "delta", MaximumProviderTextBytes);
            }
            else if (method == "item/fileChange/outputDelta" || method == "item/fileChange/patchUpdated")
            {
                event.Kind = TaskProviderEventKind::FileChanged;
                event.Text = _string(params, "delta", MaximumProviderTextBytes);
            }
            else if (method == "turn/plan/updated") event.Kind = TaskProviderEventKind::PlanUpdated;
            else if (method == "turn/diff/updated") event.Kind = TaskProviderEventKind::DiffUpdated;
            else if (method == "thread/tokenUsage/updated") event.Kind = TaskProviderEventKind::TokenUsage;
            else if (method == "turn/completed")
            {
                if (!params.isMember("turn") || !params["turn"].isObject())
                {
                    result.Status = TaskProviderStatus::MalformedResponse;
                    result.Error = "Codex turn completion is malformed";
                    return result;
                }
                const auto& turn = params["turn"];
                event.RunId = _string(turn, "id");
                const auto status = _string(turn, "status");
                event.Kind = status == "completed" ? TaskProviderEventKind::RunCompleted :
                             status == "interrupted" ? TaskProviderEventKind::RunInterrupted :
                             TaskProviderEventKind::RunFailed;
                if (turn.isMember("error") && turn["error"].isObject()) event.Text = _string(turn["error"], "message", MaximumProviderTextBytes);
            }
            else if (method == "error")
            {
                event.Kind = TaskProviderEventKind::ProviderError;
                if (params.isMember("error") && params["error"].isObject()) event.Text = _string(params["error"], "message", MaximumProviderTextBytes);
            }
            else
            {
                event.Kind = TaskProviderEventKind::UnknownOptional;
                event.Text = method;
            }

            if (event.Text.size() > MaximumProviderTextBytes)
            {
                event.Text.resize(MaximumProviderTextBytes);
                event.Truncated = true;
            }
            event.PayloadJson = _boundedPayload(params, event);
            result.Status = TaskProviderStatus::Succeeded;
            result.Event = std::move(event);
            return result;
        }
        catch (...)
        {
            result.Status = TaskProviderStatus::ResourceFailure;
            result.Error = "Codex event translation exhausted resources";
            return result;
        }
    }

    Json::Value CodexApprovalResponse(
        const CodexPendingApproval& approval,
        const TaskProviderApprovalDecision decision) noexcept
    {
        try
        {
            Json::Value result{ Json::objectValue };
            if (approval.Method == "item/permissions/requestApproval")
            {
                result["permissions"] = decision == TaskProviderApprovalDecision::Approved && approval.RequestedPermissions.isObject() ?
                                             approval.RequestedPermissions : Json::Value{ Json::objectValue };
                result["scope"] = "turn";
                return result;
            }
            result["decision"] = decision == TaskProviderApprovalDecision::Approved ? "accept" :
                                 decision == TaskProviderApprovalDecision::Denied ? "decline" : "cancel";
            return result;
        }
        catch (...)
        {
            return Json::Value{};
        }
    }
}
