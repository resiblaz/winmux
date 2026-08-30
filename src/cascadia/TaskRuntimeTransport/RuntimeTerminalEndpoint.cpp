// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalEndpoint.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        constexpr std::string_view RuntimeProtocolIdentityPrefix{
            "wt-task-runtime-v1:"
        };
        constexpr size_t RuntimeIdentityDigestCharacters{ 64 };

        RuntimeTerminalEndpointResult _failure(
            const RuntimeTerminalEndpointStatus status,
            std::string error)
        {
            RuntimeTerminalEndpointResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }

        bool _lowerHexDigest(const std::string_view value) noexcept
        {
            return value.size() == RuntimeIdentityDigestCharacters &&
                   std::ranges::all_of(value, [](const char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                   });
        }

        std::optional<std::wstring> _terminalPipeName(
            const std::string_view runtimeIdentity,
            const std::string_view terminalSessionId,
            const uint64_t generation)
        {
            if (!runtimeIdentity.starts_with(RuntimeProtocolIdentityPrefix) ||
                runtimeIdentity.size() != RuntimeProtocolIdentityPrefix.size() +
                                              RuntimeIdentityDigestCharacters ||
                !_lowerHexDigest(runtimeIdentity.substr(
                    RuntimeProtocolIdentityPrefix.size())) ||
                !Microsoft::Terminal::TaskRuntimeProtocol::IsCanonicalProtocolUuid(
                    terminalSessionId) ||
                generation == 0)
            {
                return std::nullopt;
            }

            const auto digest = runtimeIdentity.substr(
                RuntimeProtocolIdentityPrefix.size());
            std::wstring name{ L"\\\\.\\pipe\\WT.TaskRuntime." };
            for (const auto character : digest)
            {
                name.push_back(static_cast<wchar_t>(
                    static_cast<unsigned char>(character)));
            }
            name.append(L".terminal.");
            for (const auto character : terminalSessionId)
            {
                name.push_back(static_cast<wchar_t>(
                    static_cast<unsigned char>(character)));
            }
            name.push_back(L'.');
            name.append(std::to_wstring(generation));
            return name.size() <= 240 ?
                       std::optional<std::wstring>{ std::move(name) } :
                       std::nullopt;
        }

        bool _validRuntimeIdentity(const RuntimeIdentity& identity)
        {
            const auto rebuilt = BuildRuntimeIdentity(
                identity.Channel,
                identity.SettingsRoot,
                identity.UserSid);
            return rebuilt.Succeeded() &&
                   rebuilt.Identity.Channel == identity.Channel &&
                   rebuilt.Identity.SettingsRoot == identity.SettingsRoot &&
                   rebuilt.Identity.UserSid == identity.UserSid &&
                   rebuilt.Identity.ProtocolIdentity == identity.ProtocolIdentity &&
                   rebuilt.Identity.InstanceMutexName == identity.InstanceMutexName &&
                   rebuilt.Identity.LaunchMutexName == identity.LaunchMutexName &&
                   rebuilt.Identity.ControlPipeName == identity.ControlPipeName;
        }
    }

    RuntimeTerminalEndpointResult BuildRuntimeTerminalEndpoint(
        const RuntimeIdentity& runtimeIdentity,
        const std::string_view terminalSessionId,
        const uint64_t generation)
    {
        try
        {
            if (!_validRuntimeIdentity(runtimeIdentity))
            {
                return _failure(
                    RuntimeTerminalEndpointStatus::InvalidRuntimeIdentity,
                    "Terminal endpoint Runtime identity is invalid");
            }
            if (!Microsoft::Terminal::TaskRuntimeProtocol::IsCanonicalProtocolUuid(
                    terminalSessionId))
            {
                return _failure(
                    RuntimeTerminalEndpointStatus::InvalidTerminalSessionId,
                    "Terminal endpoint session identity is invalid");
            }
            if (generation == 0)
            {
                return _failure(
                    RuntimeTerminalEndpointStatus::InvalidGeneration,
                    "Terminal endpoint generation is invalid");
            }
            const auto pipeName = _terminalPipeName(
                runtimeIdentity.ProtocolIdentity,
                terminalSessionId,
                generation);
            if (!pipeName)
            {
                return _failure(
                    RuntimeTerminalEndpointStatus::InvalidRuntimeIdentity,
                    "Terminal endpoint pipe name could not be derived");
            }

            RuntimeTerminalEndpointResult result;
            result.Status = RuntimeTerminalEndpointStatus::Succeeded;
            result.Endpoint.PipeName = *pipeName;
            result.Endpoint.RuntimeIdentity = runtimeIdentity.ProtocolIdentity;
            result.Endpoint.TerminalSessionId = terminalSessionId;
            result.Endpoint.Generation = generation;
            return result;
        }
        catch (...)
        {
            return _failure(
                RuntimeTerminalEndpointStatus::ResourceFailure,
                "Terminal endpoint derivation exhausted local resources");
        }
    }

    bool ValidateRuntimeTerminalEndpoint(
        const RuntimeTerminalEndpoint& endpoint) noexcept
    {
        try
        {
            const auto expected = _terminalPipeName(
                endpoint.RuntimeIdentity,
                endpoint.TerminalSessionId,
                endpoint.Generation);
            return expected && *expected == endpoint.PipeName;
        }
        catch (...)
        {
            return false;
        }
    }
}
