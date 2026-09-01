// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CodexAppServerContract.h"

namespace Microsoft::Terminal::TaskProvider
{
    namespace
    {
        constexpr std::array<std::pair<std::string_view, TaskProviderCapability>, 11> RequiredMethods{
            std::pair{ "thread/start", TaskProviderCapability::ThreadStart },
            std::pair{ "thread/resume", TaskProviderCapability::ThreadResume },
            std::pair{ "turn/start", TaskProviderCapability::TurnStart },
            std::pair{ "turn/steer", TaskProviderCapability::TurnSteer },
            std::pair{ "turn/interrupt", TaskProviderCapability::TurnInterrupt },
            std::pair{ "thread/unsubscribe", TaskProviderCapability::ThreadUnsubscribe },
            std::pair{ "item/commandExecution/requestApproval", TaskProviderCapability::ApprovalResponse },
            std::pair{ "item/fileChange/requestApproval", TaskProviderCapability::ApprovalResponse },
            std::pair{ "item/permissions/requestApproval", TaskProviderCapability::ApprovalResponse },
            std::pair{ "item/started", TaskProviderCapability::StructuredEvents },
            std::pair{ "turn/completed", TaskProviderCapability::StructuredEvents },
        };

        bool _parse(const std::string_view value, Json::Value& root)
        {
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false;
            builder["allowTrailingCommas"] = false;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            std::string error;
            return reader && reader->parse(
                                 value.data(), value.data() + value.size(),
                                 &root, &error);
        }

        TaskProviderCapabilitiesResult _fromMethods(
            const Json::Value& methods,
            std::string version)
        {
            TaskProviderCapabilitiesResult result;
            if (!methods.isArray() || methods.size() > 4096)
            {
                result.Status = TaskProviderStatus::MalformedResponse;
                result.Error = "Codex capability method list is invalid";
                return result;
            }
            std::unordered_set<std::string> names;
            for (const auto& method : methods)
            {
                if (!method.isString() || method.asString().empty() ||
                    method.asString().size() > 128)
                {
                    result.Status = TaskProviderStatus::MalformedResponse;
                    result.Error = "Codex capability method is invalid";
                    return result;
                }
                names.emplace(method.asString());
            }
            TaskProviderCapabilities capabilities;
            capabilities.Provider = "codex";
            capabilities.ProviderVersion = std::move(version);
            capabilities.ProtocolMajor = 1;
            for (const auto& [name, capability] : RequiredMethods)
            {
                if (!names.contains(std::string{ name }))
                {
                    result.Status = TaskProviderStatus::UnsupportedCapability;
                    result.Error = "Codex App Server is missing required method: " +
                                   std::string{ name };
                    return result;
                }
                capabilities.Capabilities |= static_cast<uint64_t>(capability);
            }
            result.Status = TaskProviderStatus::Succeeded;
            result.Value = std::move(capabilities);
            return result;
        }

        bool _supportedUserAgent(std::string_view value) noexcept
        {
            constexpr std::array prefixes{
                std::string_view{ "codex_cli_rs/" },
                std::string_view{ "richer-terminal-task-runtime/" },
            };
            const auto prefix = std::ranges::find_if(prefixes, [&](const auto candidate) {
                return value.starts_with(candidate);
            });
            if (prefix == prefixes.end()) return false;
            value.remove_prefix(prefix->size());
            uint32_t major{};
            uint32_t minor{};
            uint32_t part{};
            size_t index{};
            const auto readPart = [&](uint32_t& output) {
                if (index >= value.size() || !std::isdigit(static_cast<unsigned char>(value[index]))) return false;
                uint32_t parsed{};
                while (index < value.size() && std::isdigit(static_cast<unsigned char>(value[index])))
                {
                    const auto digit = static_cast<uint32_t>(value[index++] - '0');
                    if (parsed > 10'000 || parsed * 10 + digit > 100'000) return false;
                    parsed = parsed * 10 + digit;
                }
                output = parsed;
                return true;
            };
            if (!readPart(major) || index >= value.size() || value[index++] != '.' ||
                !readPart(minor) || index >= value.size() || value[index++] != '.' ||
                !readPart(part))
            {
                return false;
            }
            static_cast<void>(part);
            return (major == 0 && minor >= 150) || major > 0;
        }
    }

    TaskProviderCapabilitiesResult ParseCodexAppServerCapabilities(
        const Json::Value& initializeResult) noexcept
    {
        try
        {
            if (!initializeResult.isObject() ||
                !initializeResult.isMember("userAgent") ||
                !initializeResult["userAgent"].isString())
            {
                TaskProviderCapabilitiesResult result;
                result.Status = TaskProviderStatus::UnsupportedVersion;
                result.Error = "Codex App Server did not identify a supported protocol version";
                return result;
            }
            const auto userAgent = initializeResult["userAgent"].asString();
            if (userAgent.empty() || userAgent.size() > 128 || !_supportedUserAgent(userAgent))
            {
                TaskProviderCapabilitiesResult result;
                result.Status = TaskProviderStatus::UnsupportedVersion;
                result.Error = "Codex App Server user agent is invalid";
                return result;
            }

            TaskProviderCapabilities capabilities;
            capabilities.Provider = "codex";
            capabilities.ProviderVersion = userAgent;
            capabilities.ProtocolMajor = 2;
            capabilities.ProtocolMinor = 0;
            capabilities.Capabilities = RequiredTaskProviderCapabilities;
            TaskProviderCapabilitiesResult result;
            result.Status = TaskProviderStatus::Succeeded;
            result.Value = std::move(capabilities);
            return result;
        }
        catch (...)
        {
            TaskProviderCapabilitiesResult result;
            result.Status = TaskProviderStatus::ResourceFailure;
            result.Error = "Codex capability parsing exhausted resources";
            return result;
        }
    }

    TaskProviderCapabilitiesResult ValidateCodexAppServerSchema(
        const std::string_view schemaJson) noexcept
    {
        try
        {
            Json::Value root;
            if (schemaJson.empty() || schemaJson.size() > 4 * 1024 * 1024 ||
                !_parse(schemaJson, root) || !root.isObject() ||
                !root.isMember("methods"))
            {
                TaskProviderCapabilitiesResult result;
                result.Status = TaskProviderStatus::MalformedResponse;
                result.Error = "Codex schema fixture is invalid";
                return result;
            }
            return _fromMethods(root["methods"], "schema-fixture");
        }
        catch (...)
        {
            TaskProviderCapabilitiesResult result;
            result.Status = TaskProviderStatus::ResourceFailure;
            result.Error = "Codex schema validation exhausted resources";
            return result;
        }
    }
}
