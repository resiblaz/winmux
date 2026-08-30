// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeRtaskCapabilityToken.h"

#include "RtaskCommand.h"

#include <bcrypt.h>

#include <array>

namespace Microsoft::Terminal::TaskRuntime
{
    std::optional<std::string> SystemRuntimeRtaskCapabilityTokenSource::Generate()
    {
        try
        {
            std::array<uint8_t, 32> random{};
            if (BCryptGenRandom(
                    nullptr,
                    random.data(),
                    static_cast<ULONG>(random.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            {
                return std::nullopt;
            }

            constexpr std::string_view digits{ "0123456789abcdef" };
            std::string token;
            token.resize(
                Microsoft::Terminal::TaskRuntimeProtocol::RtaskCapabilityBytes);
            for (size_t index = 0; index < random.size(); ++index)
            {
                token[index * 2] = digits[random[index] >> 4];
                token[index * 2 + 1] = digits[random[index] & 0x0f];
            }
            return Microsoft::Terminal::TaskRuntimeProtocol::IsValidRtaskCapability(
                       token) ?
                       std::optional{ std::move(token) } :
                       std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
