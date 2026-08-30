// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RtaskContext.h"

#include "ProtocolValidation.h"
#include "RtaskCommand.h"

namespace Microsoft::Terminal::Rtask
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        constexpr std::wstring_view EndpointPrefix{ L"\\\\.\\pipe\\WT.TaskRuntime." };
        constexpr std::wstring_view EndpointSuffix{ L".control" };
        constexpr size_t MaximumEndpointCharacters{ 256 };

        RtaskContextResult _failure(
            const RtaskContextStatus status,
            std::string error)
        {
            RtaskContextResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }

        std::optional<std::wstring> _environment(
            const std::wstring_view name,
            const size_t maximumCharacters,
            RtaskContextStatus& status,
            std::string& error)
        {
            std::array<wchar_t, MaximumEndpointCharacters + 1> buffer{};
            const auto copied = GetEnvironmentVariableW(
                std::wstring{ name }.c_str(),
                buffer.data(),
                static_cast<DWORD>((std::min)(buffer.size(), maximumCharacters + 1)));
            if (copied == 0)
            {
                status = GetLastError() == ERROR_ENVVAR_NOT_FOUND ?
                             RtaskContextStatus::MissingEnvironment :
                             RtaskContextStatus::SystemError;
                error = status == RtaskContextStatus::MissingEnvironment ?
                            "Required rtask environment is missing" :
                            "Reading rtask environment failed";
                return std::nullopt;
            }
            if (copied > maximumCharacters)
            {
                status = RtaskContextStatus::OversizedEnvironment;
                error = "Required rtask environment is oversized";
                return std::nullopt;
            }
            return std::wstring{ buffer.data(), copied };
        }

        std::optional<std::string> _ascii(const std::wstring_view value)
        {
            std::string result;
            result.reserve(value.size());
            for (const auto character : value)
            {
                if (character > 0x7f)
                {
                    return std::nullopt;
                }
                result.push_back(static_cast<char>(character));
            }
            return result;
        }
    }

    RtaskContextResult ParseRtaskContext(
        const std::wstring_view endpoint,
        const std::string_view runId,
        const std::string_view capability)
    {
        try
        {
            if (endpoint.size() != EndpointPrefix.size() + 64 + EndpointSuffix.size() ||
                !endpoint.starts_with(EndpointPrefix) ||
                !endpoint.ends_with(EndpointSuffix))
            {
                return _failure(
                    RtaskContextStatus::InvalidEndpoint,
                    "rtask endpoint is not a local Task Runtime pipe");
            }
            const auto wideHash = endpoint.substr(EndpointPrefix.size(), 64);
            std::string hash;
            hash.reserve(wideHash.size());
            for (const auto character : wideHash)
            {
                if (!((character >= L'0' && character <= L'9') ||
                      (character >= L'a' && character <= L'f')))
                {
                    return _failure(
                        RtaskContextStatus::InvalidEndpoint,
                        "rtask endpoint identity hash is invalid");
                }
                hash.push_back(static_cast<char>(character));
            }
            if (!IsCanonicalProtocolUuid(runId))
            {
                return _failure(
                    RtaskContextStatus::InvalidRun,
                    "rtask Run identity is invalid");
            }
            if (!IsValidRtaskCapability(capability))
            {
                return _failure(
                    RtaskContextStatus::InvalidCapability,
                    "rtask capability shape is invalid");
            }

            RtaskContextResult result;
            result.Context.Identity.ControlPipeName = endpoint;
            result.Context.Identity.ProtocolIdentity =
                "wt-task-runtime-v1:" + hash;
            result.Context.RunId = runId;
            result.Context.Capability = capability;
            result.Status = RtaskContextStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            return _failure(
                RtaskContextStatus::SystemError,
                "rtask context construction exhausted resources");
        }
    }

    RtaskContextResult LoadRtaskContextFromEnvironment()
    {
        RtaskContextStatus status{};
        std::string error;
        const auto endpoint = _environment(
            RtaskEndpointEnvironment,
            MaximumEndpointCharacters,
            status,
            error);
        if (!endpoint)
        {
            return _failure(status, std::move(error));
        }
        const auto run = _environment(
            RtaskRunIdEnvironment,
            36,
            status,
            error);
        if (!run)
        {
            return _failure(status, std::move(error));
        }
        const auto capability = _environment(
            RtaskCapabilityEnvironment,
            RtaskCapabilityBytes,
            status,
            error);
        if (!capability)
        {
            return _failure(status, std::move(error));
        }
        const auto runAscii = _ascii(*run);
        const auto capabilityAscii = _ascii(*capability);
        if (!runAscii)
        {
            return _failure(RtaskContextStatus::InvalidRun, "rtask Run identity is invalid");
        }
        if (!capabilityAscii)
        {
            return _failure(
                RtaskContextStatus::InvalidCapability,
                "rtask capability shape is invalid");
        }
        return ParseRtaskContext(*endpoint, *runAscii, *capabilityAscii);
    }
}
