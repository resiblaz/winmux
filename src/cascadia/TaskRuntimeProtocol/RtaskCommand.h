// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    inline constexpr size_t RtaskCapabilityBytes{ 64 };
    inline constexpr size_t MaximumRtaskBlockedReasonBytes{ 16 * 1024 };
    inline constexpr size_t MaximumRtaskBlockedReasonCharacters{ 4096 };
    inline constexpr size_t MaximumRtaskResultBytes{ 256 * 1024 };
    inline constexpr size_t MaximumRtaskResultCharacters{ 65536 };

    enum class RtaskMethod : uint8_t
    {
        StatusInProgress,
        StatusInReview,
        StatusBlocked,
        Result,
        Count,
    };

    using RtaskMethodSet = uint32_t;
    inline constexpr RtaskMethodSet AllRtaskMethods{
        (RtaskMethodSet{ 1 } << static_cast<uint8_t>(RtaskMethod::Count)) - 1
    };

    enum class RtaskCommandDisposition : uint8_t
    {
        Applied,
        Replayed,
        Count,
    };

    struct RtaskCommandRequest
    {
        std::string CommandId;
        std::string RunId;
        std::string Capability;
        RtaskMethod Method{ RtaskMethod::StatusInProgress };
        std::string Text;
    };

    struct RtaskCommandResult
    {
        std::string CommandId;
        RtaskCommandDisposition Disposition{ RtaskCommandDisposition::Applied };
        uint64_t TaskVersion{};
        uint64_t EventSequence{};
    };

    [[nodiscard]] constexpr RtaskMethodSet RtaskMethodBit(
        const RtaskMethod method) noexcept
    {
        const auto value = static_cast<uint8_t>(method);
        return value < static_cast<uint8_t>(RtaskMethod::Count) ?
                   RtaskMethodSet{ 1 } << value :
                   RtaskMethodSet{};
    }

    [[nodiscard]] constexpr bool HasRtaskMethod(
        const RtaskMethodSet methods,
        const RtaskMethod method) noexcept
    {
        const auto bit = RtaskMethodBit(method);
        return bit != 0 && (methods & bit) != 0;
    }

    [[nodiscard]] bool IsValidRtaskMethodSet(RtaskMethodSet methods) noexcept;
    [[nodiscard]] bool IsValidRtaskCapability(std::string_view value) noexcept;
    [[nodiscard]] bool ValidateRtaskCommand(
        const RtaskCommandRequest& request,
        std::string& error) noexcept;
    [[nodiscard]] bool ValidateRtaskCommandResult(
        const RtaskCommandResult& result,
        std::string& error) noexcept;
    [[nodiscard]] std::string_view RtaskMethodName(RtaskMethod method) noexcept;
    [[nodiscard]] std::optional<RtaskMethod> RtaskMethodFromName(
        std::string_view name) noexcept;
    [[nodiscard]] std::string_view RtaskCommandDispositionName(
        RtaskCommandDisposition disposition) noexcept;
    [[nodiscard]] std::optional<RtaskCommandDisposition> RtaskCommandDispositionFromName(
        std::string_view name) noexcept;
    [[nodiscard]] std::string RtaskCommandDiagnosticSummary(
        const RtaskCommandRequest& request);
}
