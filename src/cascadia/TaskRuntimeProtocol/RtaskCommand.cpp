// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RtaskCommand.h"

#include "ProtocolValidation.h"

#include <array>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        constexpr std::array<std::pair<RtaskMethod, std::string_view>, 4> MethodNames{
            std::pair{ RtaskMethod::StatusInProgress, "status_in_progress" },
            std::pair{ RtaskMethod::StatusInReview, "status_in_review" },
            std::pair{ RtaskMethod::StatusBlocked, "status_blocked" },
            std::pair{ RtaskMethod::Result, "result" },
        };

        constexpr std::array<std::pair<RtaskCommandDisposition, std::string_view>, 2> DispositionNames{
            std::pair{ RtaskCommandDisposition::Applied, "applied" },
            std::pair{ RtaskCommandDisposition::Replayed, "replayed" },
        };

        static_assert(
            static_cast<uint8_t>(RtaskMethod::Count) == MethodNames.size());
        static_assert(
            static_cast<uint8_t>(RtaskCommandDisposition::Count) ==
            DispositionNames.size());

        size_t _utf8Characters(const std::string_view value) noexcept
        {
            return static_cast<size_t>(std::ranges::count_if(value, [](const char character) {
                return (static_cast<uint8_t>(character) & 0xc0) != 0x80;
            }));
        }

        bool _validText(
            const std::string_view value,
            const size_t maximumBytes,
            const size_t maximumCharacters) noexcept
        {
            return !value.empty() && value.size() <= maximumBytes &&
                   value.find('\0') == std::string_view::npos &&
                   IsValidUtf8(value) && _utf8Characters(value) <= maximumCharacters;
        }
    }

    bool IsValidRtaskMethodSet(const RtaskMethodSet methods) noexcept
    {
        return methods != 0 && (methods & ~AllRtaskMethods) == 0;
    }

    bool IsValidRtaskCapability(const std::string_view value) noexcept
    {
        if (value.size() != RtaskCapabilityBytes)
        {
            return false;
        }
        bool nonzero{};
        for (const auto character : value)
        {
            if (!((character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f')))
            {
                return false;
            }
            nonzero = nonzero || character != '0';
        }
        return nonzero;
    }

    bool ValidateRtaskCommand(
        const RtaskCommandRequest& request,
        std::string& error) noexcept
    {
        error.clear();
        if (!IsCanonicalProtocolUuid(request.CommandId) ||
            !IsCanonicalProtocolUuid(request.RunId))
        {
            error = "rtask command identity is invalid";
            return false;
        }
        if (!IsValidRtaskCapability(request.Capability))
        {
            error = "rtask capability shape is invalid";
            return false;
        }
        if (RtaskMethodName(request.Method).empty())
        {
            error = "rtask method is invalid";
            return false;
        }

        switch (request.Method)
        {
        case RtaskMethod::StatusInProgress:
        case RtaskMethod::StatusInReview:
            if (!request.Text.empty())
            {
                error = "rtask status command does not accept text";
                return false;
            }
            return true;
        case RtaskMethod::StatusBlocked:
            if (!_validText(
                    request.Text,
                    MaximumRtaskBlockedReasonBytes,
                    MaximumRtaskBlockedReasonCharacters))
            {
                error = "rtask blocked reason is invalid or oversized";
                return false;
            }
            return true;
        case RtaskMethod::Result:
            if (!_validText(
                    request.Text,
                    MaximumRtaskResultBytes,
                    MaximumRtaskResultCharacters))
            {
                error = "rtask result is invalid or oversized";
                return false;
            }
            return true;
        default:
            error = "rtask method is invalid";
            return false;
        }
    }

    bool ValidateRtaskCommandResult(
        const RtaskCommandResult& result,
        std::string& error) noexcept
    {
        error.clear();
        if (!IsCanonicalProtocolUuid(result.CommandId) ||
            RtaskCommandDispositionName(result.Disposition).empty() ||
            result.TaskVersion == 0 || result.EventSequence == 0)
        {
            error = "rtask command result is invalid";
            return false;
        }
        return true;
    }

    std::string_view RtaskMethodName(const RtaskMethod method) noexcept
    {
        const auto found = std::ranges::find_if(MethodNames, [&](const auto& entry) {
            return entry.first == method;
        });
        return found == MethodNames.end() ? std::string_view{} : found->second;
    }

    std::optional<RtaskMethod> RtaskMethodFromName(const std::string_view name) noexcept
    {
        const auto found = std::ranges::find_if(MethodNames, [&](const auto& entry) {
            return entry.second == name;
        });
        return found == MethodNames.end() ?
                   std::nullopt :
                   std::optional<RtaskMethod>{ found->first };
    }

    std::string_view RtaskCommandDispositionName(
        const RtaskCommandDisposition disposition) noexcept
    {
        const auto found = std::ranges::find_if(DispositionNames, [&](const auto& entry) {
            return entry.first == disposition;
        });
        return found == DispositionNames.end() ? std::string_view{} : found->second;
    }

    std::optional<RtaskCommandDisposition> RtaskCommandDispositionFromName(
        const std::string_view name) noexcept
    {
        const auto found = std::ranges::find_if(DispositionNames, [&](const auto& entry) {
            return entry.second == name;
        });
        return found == DispositionNames.end() ?
                   std::nullopt :
                   std::optional<RtaskCommandDisposition>{ found->first };
    }

    std::string RtaskCommandDiagnosticSummary(const RtaskCommandRequest& request)
    {
        std::string summary;
        summary.reserve(192);
        summary.append("rtask command=");
        summary.append(
            IsCanonicalProtocolUuid(request.CommandId) ?
                std::string_view{ request.CommandId } :
                std::string_view{ "<invalid>" });
        summary.append(" run=");
        summary.append(
            IsCanonicalProtocolUuid(request.RunId) ?
                std::string_view{ request.RunId } :
                std::string_view{ "<invalid>" });
        summary.append(" method=");
        const auto method = RtaskMethodName(request.Method);
        summary.append(method.empty() ? "<invalid>" : method);
        summary.append(" text_bytes=");
        summary.append(std::to_string(request.Text.size()));
        summary.append(" capability=<redacted>");
        return summary;
    }
}
